#define _GNU_SOURCE
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdlib.h>
#include <xf86drm.h>
#include <xf86drmMode.h>
#include <drm.h>

static void probe(int fd, uint32_t id, uint32_t type, const char *label)
{
	drmModeObjectProperties *ps = drmModeObjectGetProperties(fd, id, type);
	if (!ps) return;
	printf("== %s obj=%u ==\n", label, id);
	for (uint32_t i = 0; i < ps->count_props; i++) {
		drmModePropertyRes *p = drmModeGetProperty(fd, ps->props[i]);
		drmModeAtomicReq *req = drmModeAtomicAlloc();
		int rc;
		drmModeAtomicAddProperty(req, id, ps->props[i], ps->prop_values[i]);
		rc = drmModeAtomicCommit(fd, req, DRM_MODE_ATOMIC_ALLOW_MODESET, NULL);
		if (rc)
			printf("  FAIL %-24s id=%-3u val=%llu flags=0x%x errno=%d\n",
			       p ? p->name : "?", ps->props[i],
			       (unsigned long long)ps->prop_values[i], p ? p->flags : 0, errno);
		drmModeAtomicFree(req);
		if (p) drmModeFreeProperty(p);
	}
	drmModeFreeObjectProperties(ps);
}

int main(void)
{
	int fd = open("/dev/dri/card1", O_RDWR | O_CLOEXEC);
	drmModeRes *res;
	drmModePlaneRes *pres;
	if (fd < 0) { perror("open"); return 2; }
	drmSetClientCap(fd, DRM_CLIENT_CAP_ATOMIC, 1);
	res = drmModeGetResources(fd);
	pres = drmModeGetPlaneResources(fd);
	if (!res || !pres) return 3;
	probe(fd, res->connectors[0], DRM_MODE_OBJECT_CONNECTOR, "connector");
	probe(fd, res->crtcs[0], DRM_MODE_OBJECT_CRTC, "crtc");
	if (pres->count_planes) probe(fd, pres->planes[0], DRM_MODE_OBJECT_PLANE, "plane");
	return 0;
}
