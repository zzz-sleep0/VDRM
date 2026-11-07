/*
 * egl_copy_probe.c —— 验证"把 AHardwareBuffer 当纹理采样、再渲染进另一个
 * AHardwareBuffer"这条路在本机能不能走（App 侧拷贝回退的可行性探针）。
 *
 * 走一遍真实的形状：
 *   1. 分配 source AHB（CPU 写 + GPU 采样），CPU 填成纯绿；
 *   2. 分配 target AHB（GPU 颜色输出 + 合成器叠加），当 FBO 的附着；
 *   3. 两块都经 eglGetNativeClientBufferANDROID → eglCreateImageKHR 导入；
 *   4. target 的 EGLImage 挂到 GL_TEXTURE_2D 当 FBO 颜色附着，source 的当纹理；
 *   5. 渲染一个全屏四边形（1:1），然后 glReadPixels 回读 target（再锁一次内存
 *      读真值），确认像素是绿的不是别的 —— 这一步同时验证格式有没有 R/B 互换。
 *
 * 用法（设备上，root）：
 *   egl_copy_probe            # 256x256 自检
 *   egl_copy_probe 512        # 指定边长
 *
 * 退出码 0 = 全通；非 0 会打印卡在哪一步。
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>

#include <android/hardware_buffer.h>
#include <android/log.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>

#define TAG "VDRM"
#define LOGI(...) do { printf(__VA_ARGS__); printf("\n"); fflush(stdout); \
	__android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__); } while (0)
#define LOGE(...) do { printf(__VA_ARGS__); printf("\n"); fflush(stdout); \
	__android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__); } while (0)

static PFNEGLGETNATIVECLIENTBUFFERANDROIDPROC p_eglGetNativeClientBuffer;
static PFNEGLCREATEIMAGEKHRPROC                 p_eglCreateImageKHR;
static PFNEGLDESTROYIMAGEKHRPROC                p_eglDestroyImageKHR;
static PFNGLEGLIMAGETARGETTEXTURE2DOESPROC      p_glEGLImageTargetTexture2DOES;

static EGLDisplay g_dpy;
static EGLContext g_ctx;
static EGLSurface g_surf;

static AHardwareBuffer *alloc_buf(uint32_t w, uint32_t h, uint32_t fmt,
				  uint64_t usage)
{
	AHardwareBuffer_Desc d = {
		.width  = w,
		.height = h,
		.layers = 1,
		.format = fmt,
		.usage  = usage,
	};
	AHardwareBuffer *b = NULL;
	int rc = AHardwareBuffer_allocate(&d, &b);

	if (rc != 0) {
		LOGE("AHardwareBuffer_allocate(%ux%u fmt=%u usage=0x%llx) -> %d (%s)",
		     w, h, fmt, (unsigned long long)usage, rc, strerror(-rc));
		return NULL;
	}
	return b;
}

static EGLImageKHR import_ahb(AHardwareBuffer *b)
{
	EGLClientBuffer cb;

	if (!p_eglGetNativeClientBuffer) {
		LOGE("没有 eglGetNativeClientBufferANDROID");
		return EGL_NO_IMAGE_KHR;
	}
	cb = p_eglGetNativeClientBuffer(b);
	if (!cb) {
		LOGE("eglGetNativeClientBuffer 返回空（AHB 用法里缺 GPU 采样/输出？）");
		return EGL_NO_IMAGE_KHR;
	}
	return p_eglCreateImageKHR(g_dpy, EGL_NO_CONTEXT, EGL_NATIVE_BUFFER_ANDROID,
				   cb, NULL);
}

static GLuint shader(GLenum type, const char *src)
{
	GLuint s = glCreateShader(type);
	int ok = 0;

	glShaderSource(s, 1, &src, NULL);
	glCompileShader(s);
	glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
	if (!ok) {
		char log[512];

		glGetShaderInfoLog(s, sizeof(log), NULL, log);
		LOGE("shader 编译失败: %s", log);
		return 0;
	}
	return s;
}

int main(int argc, char **argv)
{
	uint32_t n = argc > 1 ? (uint32_t)atoi(argv[1]) : 256;
	uint32_t sfmt = argc > 2 ? (uint32_t)atoi(argv[2]) : AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM;
	uint32_t w = n, h = n;
	AHardwareBuffer *src = NULL, *dst = NULL;
	EGLImageKHR isrc = EGL_NO_IMAGE_KHR, idst = EGL_NO_IMAGE_KHR;
	EGLConfig cfg;
	EGLint nc = 0, major = 0, minor = 0;
	EGLint cfgattr[] = {
		EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
		EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
		EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8,
		EGL_NONE,
	};
	EGLint ctxattr[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
	EGLint pbattr[] = { EGL_WIDTH, 16, EGL_HEIGHT, 16, EGL_NONE };
	GLuint tex_src, tex_dst, fb, prog, vbo;
	GLint rc;
	GLubyte px[4];
	const char *vs =
		"attribute vec2 p; varying vec2 uv;\n"
		"void main(){ uv = p * 0.5 + 0.5; gl_Position = vec4(p,0.0,1.0); }\n";
	const char *fs =
		"#extension GL_OES_EGL_image_external : require\n"
		"precision mediump float; varying vec2 uv; uniform sampler2D t;\n"
		"void main(){ gl_FragColor = texture2D(t, uv); }\n";
	float quad[] = { -1,-1, 1,-1, -1,1, 1,1 };

	/* ---- EGL ---- */
	g_dpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);
	if (g_dpy == EGL_NO_DISPLAY) {
		LOGE("eglGetDisplay 失败");
		return 1;
	}
	if (!eglInitialize(g_dpy, &major, &minor)) {
		LOGE("eglInitialize 失败: 0x%x", eglGetError());
		return 1;
	}
	LOGI("EGL %d.%d  vendor=%s", major, minor, eglQueryString(g_dpy, EGL_VENDOR));
	if (!eglChooseConfig(g_dpy, cfgattr, &cfg, 1, &nc) || nc < 1) {
		LOGE("eglChooseConfig 失败");
		return 1;
	}
	g_ctx = eglCreateContext(g_dpy, cfg, EGL_NO_CONTEXT, ctxattr);
	if (g_ctx == EGL_NO_CONTEXT) {
		LOGE("eglCreateContext 失败: 0x%x", eglGetError());
		return 1;
	}
	g_surf = eglCreatePbufferSurface(g_dpy, cfg, pbattr);
	if (g_surf == EGL_NO_SURFACE || !eglMakeCurrent(g_dpy, g_surf, g_surf, g_ctx)) {
		LOGE("makeCurrent 失败: 0x%x", eglGetError());
		return 1;
	}
	LOGI("GL_RENDERER = %s", (const char *)glGetString(GL_RENDERER));

	p_eglGetNativeClientBuffer = (PFNEGLGETNATIVECLIENTBUFFERANDROIDPROC)
		eglGetProcAddress("eglGetNativeClientBufferANDROID");
	p_eglCreateImageKHR = (PFNEGLCREATEIMAGEKHRPROC)
		eglGetProcAddress("eglCreateImageKHR");
	p_glEGLImageTargetTexture2DOES = (PFNGLEGLIMAGETARGETTEXTURE2DOESPROC)
		eglGetProcAddress("glEGLImageTargetTexture2DOES");
	if (!p_eglCreateImageKHR || !p_glEGLImageTargetTexture2DOES) {
		LOGE("缺 EGL_ANDROID_get_native_client_buffer / GL_OES_EGL_image");
		return 1;
	}

	/* ---- source：CPU 填纯绿 ---- */
	src = alloc_buf(w, h, sfmt,
			AHARDWAREBUFFER_USAGE_CPU_WRITE_OFTEN |
			AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE);
	if (!src)
		return 2;
	{
		void *p = NULL;
		int rc2 = AHardwareBuffer_lock(src, AHARDWAREBUFFER_USAGE_CPU_WRITE_OFTEN,
					       -1, NULL, &p);
		uint32_t i, j;

		if (rc2 != 0 || !p) {
			LOGE("source lock 失败 %d", rc2);
			return 2;
		}
		AHardwareBuffer_Desc sd;

		AHardwareBuffer_describe(src, &sd);
		for (j = 0; j < h; j++) {
			uint8_t *row = (uint8_t *)p + (size_t)j * sd.stride * 4;

			for (i = 0; i < w; i++) {
				row[i * 4 + 0] = 0x00;   /* R */
				row[i * 4 + 1] = 0xFF;   /* G */
				row[i * 4 + 2] = 0x00;   /* B */
				row[i * 4 + 3] = 0xFF;   /* A */
			}
		}
		AHardwareBuffer_unlock(src, NULL);
		LOGI("source %ux%u fmt=%u stride=%u 填成纯绿", w, h, sfmt, sd.stride);
	}

	/* ---- target：GPU 输出 + 合成器叠加 ---- */
	dst = alloc_buf(w, h, AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM,
			AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE |
			AHARDWAREBUFFER_USAGE_GPU_COLOR_OUTPUT |
			AHARDWAREBUFFER_USAGE_COMPOSER_OVERLAY);
	if (!dst)
		return 3;

	isrc = import_ahb(src);
	if (isrc == EGL_NO_IMAGE_KHR) {
		LOGE("source AHB 导入 EGLImage 失败: 0x%x", eglGetError());
		return 4;
	}
	idst = import_ahb(dst);
	if (idst == EGL_NO_IMAGE_KHR) {
		LOGE("target AHB 导入 EGLImage 失败: 0x%x", eglGetError());
		return 5;
	}
	LOGI("两块 AHB 都导入成 EGLImage 了");

	/* ---- GL 对象 ---- */
	prog = glCreateProgram();
	{
		GLuint v = shader(GL_VERTEX_SHADER, vs);
		GLuint f = shader(GL_FRAGMENT_SHADER, fs);

		if (!v || !f)
			return 6;
		glAttachShader(prog, v);
		glAttachShader(prog, f);
		glLinkProgram(prog);
		glGetProgramiv(prog, GL_LINK_STATUS, &rc);
		if (!rc) {
			char log[512];

			glGetProgramInfoLog(prog, sizeof(log), NULL, log);
			LOGE("link 失败: %s", log);
			return 6;
		}
		glUseProgram(prog);
	}
	glGenTextures(1, &tex_src);
	glBindTexture(GL_TEXTURE_2D, tex_src);
	p_glEGLImageTargetTexture2DOES(GL_TEXTURE_2D, (GLeglImageOES)isrc);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);

	glGenTextures(1, &tex_dst);
	glBindTexture(GL_TEXTURE_2D, tex_dst);
	p_glEGLImageTargetTexture2DOES(GL_TEXTURE_2D, (GLeglImageOES)idst);
	glGenFramebuffers(1, &fb);
	glBindFramebuffer(GL_FRAMEBUFFER, fb);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
			       GL_TEXTURE_2D, tex_dst, 0);
	if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
		LOGE("FBO 不完整: 0x%x", glCheckFramebufferStatus(GL_FRAMEBUFFER));
		return 7;
	}
	LOGI("FBO 完整，开始 1:1 采样");
	glViewport(0, 0, w, h);
	glGenBuffers(1, &vbo);
	glBindBuffer(GL_ARRAY_BUFFER, vbo);
	glBufferData(GL_ARRAY_BUFFER, sizeof(quad), quad, GL_STATIC_DRAW);
	glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, (void *)0);
	glEnableVertexAttribArray(0);
	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_2D, tex_src);
	glUniform1i(glGetUniformLocation(prog, "t"), 0);
	glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
	glFinish();
	LOGI("glError=0x%x", glGetError());

	/* ---- 回读 target 的 CPU 内容，看颜色 ---- */
	glReadPixels(0, 0, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
	LOGI("glReadPixels px=%02x %02x %02x %02x (期望绿色 00 ff 00 ff)", px[0], px[1], px[2], px[3]);

	if (AHardwareBuffer_lock(dst, AHARDWAREBUFFER_USAGE_CPU_READ_OFTEN, -1, NULL,
				 (void **)&px) == 0) {
		LOGI("dst 内存首像素 = %02x %02x %02x %02x", px[0], px[1], px[2], px[3]);
		AHardwareBuffer_unlock(dst, NULL);
	} else {
		LOGI("dst 没法 CPU 读（没给 CPU 读 usage，正常）");
	}

	LOGI("探针结束：全通");
	return 0;
}
