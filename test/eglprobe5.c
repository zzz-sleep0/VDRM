#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <limits.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <gbm.h>

static void dump_fds(void)
{
	DIR *d = opendir("/proc/self/fd");
	struct dirent *e;
	char p[64], t[PATH_MAX];

	while ((e = readdir(d))) {
		if (e->d_name[0] == '.') continue;
		snprintf(p, sizeof(p), "/proc/self/fd/%s", e->d_name);
		ssize_t n = readlink(p, t, sizeof(t) - 1);
		if (n <= 0) continue;
		t[n] = 0;
		if (!strncmp(t, "/dev/", 5)) printf("  fd %s -> %s\n", e->d_name, t);
	}
	closedir(d);
}

int main(int argc, char **argv)
{
	int fd = open(argv[1], O_RDWR | O_CLOEXEC);
	if (fd < 0) { perror("open"); return 2; }
	struct gbm_device *gbm = gbm_create_device(fd);
	if (!gbm) { printf("gbm_create_device=FAIL\n"); return 3; }
	PFNEGLGETPLATFORMDISPLAYEXTPROC getpd =
		(PFNEGLGETPLATFORMDISPLAYEXTPROC)eglGetProcAddress("eglGetPlatformDisplayEXT");
	EGLDisplay dpy = getpd(EGL_PLATFORM_GBM_KHR, gbm, NULL);
	EGLint maj, min;
	if (!eglInitialize(dpy, &maj, &min)) { printf("eglInitialize=FAIL 0x%x\n", eglGetError()); return 4; }
	eglBindAPI(EGL_OPENGL_ES_API);
	EGLint ca[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
	EGLContext ctx = eglCreateContext(dpy, EGL_NO_CONFIG_KHR, EGL_NO_CONTEXT, ca);
	if (!ctx || !eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, ctx)) {
		printf("ctx/makecurrent FAIL 0x%x\n", eglGetError()); return 5;
	}
	printf("GL_RENDERER=%s\n", glGetString(GL_RENDERER));
	GLuint tex = 0, fbo = 0;
	unsigned char px[4] = {0};
	glGenTextures(1, &tex);
	glBindTexture(GL_TEXTURE_2D, tex);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 64, 64, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
	glGenFramebuffers(1, &fbo);
	glBindFramebuffer(GL_FRAMEBUFFER, fbo);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
	glViewport(0, 0, 64, 64);
	glClearColor(0.25f, 0.5f, 0.75f, 1.0f);
	glClear(GL_COLOR_BUFFER_BIT);
	glReadPixels(0, 0, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
	printf("readback=%02x%02x%02x%02x glerr=0x%x\n", px[0], px[1], px[2], px[3],
	       glGetError());
	dump_fds();
	return 0;
}
