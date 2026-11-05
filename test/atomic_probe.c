#define _GNU_SOURCE
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <xf86drm.h>
#include <xf86drmMode.h>
#include <drm.h>

static int find_prop(int fd, uint32_t obj, uint32_t type, const char *name)
{
	drmModeObjectProperties *ps = drmModeObjectGetProperties(fd, obj, type);
	int id = 0;
	if (!ps) return 0;
	for (uint32_t i = 0; i < ps->count_props; i++) {
		drmModePropertyRes *p = drmModeGetProperty(fd, ps->props[i]);
		if (p && !strcmp(p->name, name)) id = p->prop_id;
		if (p) drmModeFreeProperty(p);
		if (id) break;
	}
	drmModeFreeObjectProperties(ps);
	return id;
}

int main(void)
{
	int fd = open("/dev/dri/card1", O_RDWR | O_CLOEXEC);
	drmModeRes *res;
	drmModePlaneRes *pres;
	int rc;

	if (fd < 0) { perror("open"); return 2; }
	printf("set_client_cap(atomic) = %d\n", drmSetClientCap(fd, DRM_CLIENT_CAP_ATOMIC, 1));
	res = drmModeGetResources(fd);
	pres = drmModeGetPlaneResources(fd);
	if (!res || !pres) { printf("resources failed\n"); return 3; }
	printf("crtc=%u conn=%u plane=%u\n", res->crtcs[0], res->connectors[0],
	       pres->count_planes ? pres->planes[0] : 0);

	/* A: 空提交 */
	{
		drmModeAtomicReq *req = drmModeAtomicAlloc();
		rc = drmModeAtomicCommit(fd, req, DRM_MODE_ATOMIC_ALLOW_MODESET, NULL);
		printf("A empty+ALLOW_MODESET rc=%d errno=%d\n", rc, errno);
		drmModeAtomicFree(req);
	}
	/* B: 每个 CRTC ACTIVE=0 */
	{
		drmModeAtomicReq *req = drmModeAtomicAlloc();
		for (int i = 0; i < res->count_crtcs; i++) {
			int p = find_prop(fd, res->crtcs[i], DRM_MODE_OBJECT_CRTC, "ACTIVE");
			if (p) drmModeAtomicAddProperty(req, res->crtcs[i], p, 0);
		}
		rc = drmModeAtomicCommit(fd, req, DRM_MODE_ATOMIC_ALLOW_MODESET, NULL);
		printf("B crtc ACTIVE=0 rc=%d errno=%d\n", rc, errno);
		drmModeAtomicFree(req);
	}
	/* C: CRTC MODE_ID=0 + ACTIVE=0 */
	{
		drmModeAtomicReq *req = drmModeAtomicAlloc();
		int pa = find_prop(fd, res->crtcs[0], DRM_MODE_OBJECT_CRTC, "ACTIVE");
		int pm = find_prop(fd, res->crtcs[0], DRM_MODE_OBJECT_CRTC, "MODE_ID");
		if (pm) drmModeAtomicAddProperty(req, res->crtcs[0], pm, 0);
		if (pa) drmModeAtomicAddProperty(req, res->crtcs[0], pa, 0);
		rc = drmModeAtomicCommit(fd, req, DRM_MODE_ATOMIC_ALLOW_MODESET, NULL);
		printf("C crtc MODE_ID=0,ACTIVE=0 rc=%d errno=%d\n", rc, errno);
		drmModeAtomicFree(req);
	}
	/* D: plane FB_ID=0 */
	if (pres->count_planes) {
		drmModeAtomicReq *req = drmModeAtomicAlloc();
		int pf = find_prop(fd, pres->planes[0], DRM_MODE_OBJECT_PLANE, "FB_ID");
		int pc = find_prop(fd, pres->planes[0], DRM_MODE_OBJECT_PLANE, "CRTC_ID");
		if (pf) drmModeAtomicAddProperty(req, pres->planes[0], pf, 0);
		if (pc) drmModeAtomicAddProperty(req, pres->planes[0], pc, 0);
		rc = drmModeAtomicCommit(fd, req, DRM_MODE_ATOMIC_ALLOW_MODESET, NULL);
		printf("D plane FB_ID=0,CRTC_ID=0 rc=%d errno=%d\n", rc, errno);
		drmModeAtomicFree(req);
	}
	/* E: connector CRTC_ID=0 */
	{
		drmModeAtomicReq *req = drmModeAtomicAlloc();
		int pc = find_prop(fd, res->connectors[0], DRM_MODE_OBJECT_CONNECTOR, "CRTC_ID");
		if (pc) drmModeAtomicAddProperty(req, res->connectors[0], pc, 0);
		rc = drmModeAtomicCommit(fd, req, DRM_MODE_ATOMIC_ALLOW_MODESET, NULL);
		printf("E connector CRTC_ID=0 rc=%d errno=%d\n", rc, errno);
		drmModeAtomicFree(req);
	}
	return 0;
}
