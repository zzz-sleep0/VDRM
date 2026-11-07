/*
 * render_copy.c —— App 侧的 EGL 拷贝回退（anland 架构里"超速就拷贝"的那一半）。
 *
 * 为什么需要它：内核那边 flip-done 现在是**每个 vsync tick 无条件发**的
 * （合成的节拍来自我们自己的假 vblank，不等 SF 的 present —— 等它就会把
 * 合成器串行化，表现为"过一阵卡一下"）。代价是合成器拿到 flip-done 后
 * 可能立刻重画一块 SF 还在扫描的显存。
 *
 * 这个模块就是把那一帧的内容 blit 到我们自己的 AHardwareBuffer 里再交给
 * SF：SF 拿的是拷贝，池槽随合成器怎么重画都不会撕。池槽在 blit 完成后
 * 就可以立刻 RETURN 给内核（内容已经被消费掉了）。
 *
 * 实测前提（test/egl_copy_probe.c，本机 Adreno 750）：
 *   - 两块 AHB 都能 eglGetNativeClientBufferANDROID → eglCreateImageKHR；
 *   - EGLImage 挂 GL_TEXTURE_2D 既当纹理也能当 FBO 颜色附着；
 *   - 1:1 采样回读颜色正确（RGBA_8888 与 BGRA_8888 源都试过）。
 *
 * 线程模型：只有一个 latch 线程会调 rcopy_blit()，EGL context 就在那个线程
 * 上建、make current 一次；目标缓冲的回收来自 release 回调线程，所以
 * acquire / give_back 用一把锁。
 */
#define LOG_TAG "VDRM"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>

#include <android/log.h>
#include <android/hardware_buffer.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>

#include "render_copy.h"

#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  "VDRM", __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN,  "VDRM", __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, "VDRM", __VA_ARGS__)

#define RCOPY_TARGETS 5      /* 目标环：SF 手上最多 3 块，留两块给"在画/在等" */
#define RCOPY_SRC_CACHE 8    /* 源（池槽 AHB）→ EGLImage 的缓存 */

struct rcopy_target {
	AHardwareBuffer *ahb;
	EGLImageKHR      img;
	GLuint           tex;
	GLuint           fbo;
	bool             busy;
};

struct rcopy_src {
	AHardwareBuffer *ahb;
	EGLImageKHR      img;
	GLuint           tex;
	unsigned long    use;
};

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;

static EGLDisplay g_dpy = EGL_NO_DISPLAY;
static EGLContext g_ctx = EGL_NO_CONTEXT;
static EGLSurface g_surf = EGL_NO_SURFACE;
static GLuint     g_prog, g_vbo;
static GLint      g_u_tex;
static uint32_t   g_w, g_h, g_fmt;
static bool       g_ready;
static pthread_t  g_owner;

static unsigned long long blit_t0;
static unsigned long long mono_us(void);
static struct rcopy_target g_tgt[RCOPY_TARGETS];
static struct rcopy_src    g_src[RCOPY_SRC_CACHE];
static unsigned long       g_src_tick;

/* 诊断 */
static unsigned long g_frames, g_fail, g_giveback;
static unsigned long g_blit_max_us, g_blit_last_us;

static PFNEGLGETNATIVECLIENTBUFFERANDROIDPROC p_get_nc_buffer;
static PFNEGLCREATEIMAGEKHRPROC              p_create_img;
static PFNGLEGLIMAGETARGETTEXTURE2DOESPROC   p_img_to_tex;
static PFNEGLDESTROYIMAGEKHRPROC             p_destroy_img;

static unsigned long long mono_us(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (unsigned long long)ts.tv_sec * 1000000ull +
	       (unsigned long long)(ts.tv_nsec / 1000);
}

/* ---------------- EGL / GL ---------------- */

static GLuint make_shader(GLenum type, const char *src)
{
	GLuint s = glCreateShader(type);
	GLint ok = 0;

	glShaderSource(s, 1, &src, NULL);
	glCompileShader(s);
	glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
	if (!ok) {
		char log[512];

		glGetShaderInfoLog(s, sizeof(log), NULL, log);
		LOGE("rcopy: shader 编译失败: %s", log);
		glDeleteShader(s);
		return 0;
	}
	return s;
}

static bool gl_init(void)
{
	EGLint major = 0, minor = 0, n = 0;
	EGLConfig cfg;
	EGLint cfgattr[] = {
		EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
		EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
		EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8,
		EGL_NONE,
	};
	EGLint ctxattr[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
	EGLint pbattr[]  = { EGL_WIDTH, 16, EGL_HEIGHT, 16, EGL_NONE };
	const char *vs =
		"attribute vec2 p; varying vec2 uv;\n"
		"void main(){ uv = p * 0.5 + 0.5; gl_Position = vec4(p, 0.0, 1.0); }\n";
	const char *fs =
		"precision mediump float; varying vec2 uv; uniform sampler2D t;\n"
		"void main(){ gl_FragColor = texture2D(t, uv); }\n";
	float quad[] = { -1, -1, 1, -1, -1, 1, 1, 1 };
	GLuint v, f;
	GLint linked = 0;

	g_dpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);
	if (g_dpy == EGL_NO_DISPLAY || !eglInitialize(g_dpy, &major, &minor)) {
		LOGE("rcopy: eglInitialize 失败 0x%x", eglGetError());
		goto fail;
	}
	p_get_nc_buffer = (PFNEGLGETNATIVECLIENTBUFFERANDROIDPROC)
		eglGetProcAddress("eglGetNativeClientBufferANDROID");
	p_create_img = (PFNEGLCREATEIMAGEKHRPROC)
		eglGetProcAddress("eglCreateImageKHR");
	p_img_to_tex = (PFNGLEGLIMAGETARGETTEXTURE2DOESPROC)
		eglGetProcAddress("glEGLImageTargetTexture2DOES");
	p_destroy_img = (PFNEGLDESTROYIMAGEKHRPROC)
		eglGetProcAddress("eglDestroyImageKHR");
	if (!p_get_nc_buffer || !p_create_img || !p_img_to_tex) {
		LOGE("rcopy: 缺 EGL_ANDROID_get_native_client_buffer / OES_EGL_image");
		goto fail;
	}
	if (!eglChooseConfig(g_dpy, cfgattr, &cfg, 1, &n) || n < 1) {
		LOGE("rcopy: eglChooseConfig 失败");
		goto fail;
	}
	g_ctx = eglCreateContext(g_dpy, cfg, EGL_NO_CONTEXT, ctxattr);
	g_surf = eglCreatePbufferSurface(g_dpy, cfg, pbattr);
	if (g_ctx == EGL_NO_CONTEXT || g_surf == EGL_NO_SURFACE ||
	    !eglMakeCurrent(g_dpy, g_surf, g_surf, g_ctx)) {
		LOGE("rcopy: context/makeCurrent 失败 0x%x", eglGetError());
		goto fail;
	}
	g_owner = pthread_self();
	LOGI("rcopy: EGL %d.%d, GL_RENDERER=%s", major, minor,
	     (const char *)glGetString(GL_RENDERER));

	v = make_shader(GL_VERTEX_SHADER, vs);
	f = make_shader(GL_FRAGMENT_SHADER, fs);
	if (!v || !f)
		goto fail;
	g_prog = glCreateProgram();
	glAttachShader(g_prog, v);
	glAttachShader(g_prog, f);
	glLinkProgram(g_prog);
	glGetProgramiv(g_prog, GL_LINK_STATUS, &linked);
	if (!linked) {
		char log[512];

		glGetProgramInfoLog(g_prog, sizeof(log), NULL, log);
		LOGE("rcopy: link 失败: %s", log);
		goto fail;
	}
	g_u_tex = glGetUniformLocation(g_prog, "t");
	glGenBuffers(1, &g_vbo);
	glBindBuffer(GL_ARRAY_BUFFER, g_vbo);
	glBufferData(GL_ARRAY_BUFFER, sizeof(quad), quad, GL_STATIC_DRAW);
	return true;

fail:
	return false;
}

static void ring_destroy(void)
{
	int i;

	for (i = 0; i < RCOPY_TARGETS; i++) {
		struct rcopy_target *t = &g_tgt[i];

		if (t->fbo)
			glDeleteFramebuffers(1, &t->fbo);
		if (t->tex)
			glDeleteTextures(1, &t->tex);
		if (t->img != EGL_NO_IMAGE_KHR && p_destroy_img)
			p_destroy_img(g_dpy, t->img);
		if (t->ahb)
			AHardwareBuffer_release(t->ahb);
		memset(t, 0, sizeof(*t));
		t->img = EGL_NO_IMAGE_KHR;
	}
	for (i = 0; i < RCOPY_SRC_CACHE; i++) {
		struct rcopy_src *s = &g_src[i];

		if (s->tex)
			glDeleteTextures(1, &s->tex);
		if (s->img != EGL_NO_IMAGE_KHR && p_destroy_img)
			p_destroy_img(g_dpy, s->img);
		memset(s, 0, sizeof(*s));
		s->img = EGL_NO_IMAGE_KHR;
	}
}

static AHardwareBuffer *alloc_target(uint32_t w, uint32_t h, uint32_t fmt)
{
	AHardwareBuffer_Desc d = {
		.width  = w,
		.height = h,
		.layers = 1,
		.format = fmt,
		.usage  = AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE |
			  AHARDWAREBUFFER_USAGE_GPU_COLOR_OUTPUT |
			  AHARDWAREBUFFER_USAGE_COMPOSER_OVERLAY,
	};
	AHardwareBuffer *b = NULL;
	int rc = AHardwareBuffer_allocate(&d, &b);

	if (rc != 0) {
		LOGE("rcopy: allocate %ux%u fmt=%u 失败 %d", w, h, fmt, rc);
		return NULL;
	}
	return b;
}

static bool ring_create(uint32_t w, uint32_t h, uint32_t fmt)
{
	int i;

	for (i = 0; i < RCOPY_TARGETS; i++) {
		struct rcopy_target *t = &g_tgt[i];
		EGLClientBuffer cb;

		t->img = EGL_NO_IMAGE_KHR;
		t->ahb = alloc_target(w, h, fmt);
		if (!t->ahb)
			goto fail;
		cb = p_get_nc_buffer(t->ahb);
		if (!cb) {
			LOGE("rcopy: target %d 的 eglGetNativeClientBuffer 失败", i);
			goto fail;
		}
		t->img = p_create_img(g_dpy, EGL_NO_CONTEXT,
				      EGL_NATIVE_BUFFER_ANDROID, cb, NULL);
		if (t->img == EGL_NO_IMAGE_KHR) {
			LOGE("rcopy: target %d EGLImage 失败 0x%x", i, eglGetError());
			goto fail;
		}
		glGenTextures(1, &t->tex);
		glBindTexture(GL_TEXTURE_2D, t->tex);
		p_img_to_tex(GL_TEXTURE_2D, (GLeglImageOES)t->img);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
		glGenFramebuffers(1, &t->fbo);
		glBindFramebuffer(GL_FRAMEBUFFER, t->fbo);
		glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
				       GL_TEXTURE_2D, t->tex, 0);
		if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
			LOGE("rcopy: target %d FBO 不完整", i);
			goto fail;
		}
	}
	glBindFramebuffer(GL_FRAMEBUFFER, 0);
	LOGI("rcopy: 目标环就绪 %ux%u fmt=%u ×%d", w, h, fmt, RCOPY_TARGETS);
	return true;

fail:
	ring_destroy();
	return false;
}

/* ---------------- 对外接口 ---------------- */

bool rcopy_ensure(uint32_t w, uint32_t h, uint32_t fmt)
{
	bool ok = true;

	pthread_mutex_lock(&g_lock);
	if (!g_ready) {
		if (!gl_init()) {
			pthread_mutex_unlock(&g_lock);
			return false;
		}
		g_ready = true;
	}
	if (g_w != w || g_h != h || g_fmt != fmt || !g_tgt[0].ahb) {
		ring_destroy();
		g_w = w; g_h = h; g_fmt = fmt;
		ok = ring_create(w, h, fmt);
		if (!ok) {
			g_w = g_h = g_fmt = 0;
			g_ready = false;
		}
	}
	pthread_mutex_unlock(&g_lock);
	return ok;
}

AHardwareBuffer *rcopy_acquire(uint32_t *w, uint32_t *h)
{
	AHardwareBuffer *b = NULL;
	int i;

	pthread_mutex_lock(&g_lock);
	for (i = 0; i < RCOPY_TARGETS; i++) {
		if (!g_tgt[i].busy && g_tgt[i].ahb) {
			g_tgt[i].busy = true;
			b = g_tgt[i].ahb;
			break;
		}
	}
	if (w)
		*w = g_w;
	if (h)
		*h = g_h;
	pthread_mutex_unlock(&g_lock);
	return b;
}

void rcopy_give_back(AHardwareBuffer *dst)
{
	int i;

	if (!dst)
		return;
	pthread_mutex_lock(&g_lock);
	for (i = 0; i < RCOPY_TARGETS; i++) {
		if (g_tgt[i].ahb == dst) {
			g_tgt[i].busy = false;
			g_giveback++;
			break;
		}
	}
	pthread_mutex_unlock(&g_lock);
}

/* 把 src 的内容 1:1 blit 到 dst。失败返回 false（调用方退回零拷贝）。*/
bool rcopy_blit(AHardwareBuffer *src, uint32_t sw, uint32_t sh,
		AHardwareBuffer *dst, uint32_t dw, uint32_t dh)
{
	struct rcopy_src *sc = NULL;
	struct rcopy_target *tg = NULL;
	EGLClientBuffer cb;
	int i, victim = 0;
	unsigned long oldest = ~0ul;

	if (!src || !dst || !g_ready)
		return false;
	/* EGL context 同一时刻只能 current 在一个线程上，但**可以在线程间迁移**。
	 * 以前这里要求"必须是在建上下文的那个线程上调用"，实测 App 的 latch 会
	 * 在不同线程间切（首帧和之后不是同一个），于是每一帧都被拒 → 全退零拷贝，
	 * 还伴随 2 秒级的停帧。现在：谁调用就把上下文 make 到谁身上
	 * （g_lock 已经把并发挡掉了）。*/
	if (!pthread_equal(pthread_self(), g_owner)) {
		if (!eglMakeCurrent(g_dpy, g_surf, g_surf, g_ctx)) {
			LOGE("rcopy: eglMakeCurrent 迁移失败 0x%x", eglGetError());
			return false;
		}
		g_owner = pthread_self();
	}

	pthread_mutex_lock(&g_lock);

	for (i = 0; i < RCOPY_SRC_CACHE; i++) {
		if (g_src[i].ahb == src) {
			sc = &g_src[i];
			break;
		}
		if (g_src[i].use < oldest) {
			oldest = g_src[i].use;
			victim = i;
		}
	}
	if (!sc) {
		sc = &g_src[victim];
		if (sc->tex)
			glDeleteTextures(1, &sc->tex);
		if (sc->img != EGL_NO_IMAGE_KHR && p_destroy_img)
			p_destroy_img(g_dpy, sc->img);
		memset(sc, 0, sizeof(*sc));
		sc->img = EGL_NO_IMAGE_KHR;
		cb = p_get_nc_buffer(src);
		if (!cb) {
			pthread_mutex_unlock(&g_lock);
			LOGW("rcopy: 源 AHB 的 eglGetNativeClientBuffer 失败");
			return false;
		}
		sc->img = p_create_img(g_dpy, EGL_NO_CONTEXT,
				       EGL_NATIVE_BUFFER_ANDROID, cb, NULL);
		if (sc->img == EGL_NO_IMAGE_KHR) {
			pthread_mutex_unlock(&g_lock);
			LOGW("rcopy: 源 EGLImage 失败 0x%x", eglGetError());
			return false;
		}
		glGenTextures(1, &sc->tex);
		glBindTexture(GL_TEXTURE_2D, sc->tex);
		p_img_to_tex(GL_TEXTURE_2D, (GLeglImageOES)sc->img);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
		sc->ahb = src;
	}
	sc->use = ++g_src_tick;

	for (i = 0; i < RCOPY_TARGETS; i++) {
		if (g_tgt[i].ahb == dst) {
			tg = &g_tgt[i];
			break;
		}
	}
	if (!tg) {
		pthread_mutex_unlock(&g_lock);
		LOGW("rcopy: dst 不在环里");
		return false;
	}

	{
		unsigned long long t0 = mono_us();

		blit_t0 = t0;
	}
	glBindFramebuffer(GL_FRAMEBUFFER, tg->fbo);
	glViewport(0, 0, (GLsizei)dw, (GLsizei)dh);
	glUseProgram(g_prog);
	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_2D, sc->tex);
	glUniform1i(g_u_tex, 0);
	glBindBuffer(GL_ARRAY_BUFFER, g_vbo);
	glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, (void *)0);
	glEnableVertexAttribArray(0);
	glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
	/* 必须在把池槽 RETURN 给内核之前完成：那块显存随时可能被别人画。
	 * 先 glFlush + fence 等待；实测这一屏的 blit 很短，先按最稳的来。*/
	glFinish();
	{
		unsigned long dt = (unsigned long)(mono_us() - blit_t0);

		g_blit_last_us = dt;
		if (dt > g_blit_max_us)
			g_blit_max_us = dt;
	}
	glBindFramebuffer(GL_FRAMEBUFFER, 0);

	pthread_mutex_unlock(&g_lock);
	g_frames++;
	return true;
}

void rcopy_counters(unsigned long *frames, unsigned long *fails,
		    unsigned long *givebacks, int *free_targets)
{
	int i, free_n = 0;

	pthread_mutex_lock(&g_lock);
	for (i = 0; i < RCOPY_TARGETS; i++)
		if (!g_tgt[i].busy)
			free_n++;
	*frames = g_frames;
	*fails = g_fail;
	*givebacks = g_giveback;
	*free_targets = free_n;
	pthread_mutex_unlock(&g_lock);
}

unsigned long rcopy_blit_max_us(void)
{
	return g_blit_max_us;
}

unsigned long rcopy_blit_last_us(void)
{
	return g_blit_last_us;
}
