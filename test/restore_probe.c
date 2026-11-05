/* 复现 smithay 退出时的 "restore previous state"：把启动时快照的所有属性
 * 原样再提交一次。用来定位到底哪个属性/对象让内核返回 EINVAL。 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <xf86drm.h>
#include <xf86drmMode.h>
#include <drm.h>
#include <stdlib.h>

struct obj { uint32_t id, type; };

static int commit(int fd, drmModeAtomicReq *req, const char *what)
{
	int rc = drmModeAtomicCommit(fd, req, DRM_MODE_ATOMIC_ALLOW_MODESET, NULL);
	printf("  %-52s rc=%d errno=%d\n", what, rc, rc ? errno : 0);
	return rc;
}

static unsigned add_props(int fd, drmModeAtomicReq *req, struct obj o, int immutable_only,
			  int want_immutable)
{
	drmModeObjectProperties *ps = drmModeObjectGetProperties(fd, o.id, o.type);
	unsigned n = 0;
	if (!ps) return 0;
	for (uint32_t i = 0; i < ps->count_props; i++) {
		drmModePropertyRes *p = drmModeGetProperty(fd, ps->props[i]);
		int imm = p && (p->flags & DRM_MODE_PROP_IMMUTABLE);
		if (immutable_only && imm != want_immutable) {
			drmModeFreeProperty(p);
			continue;
		}
		if (p && drmModeAtomicAddProperty(req, o.id, ps->props[i], ps->prop_values[i]) >= 0) {
			if (getenv("PROBE_VERBOSE"))
				printf("     add obj=%u prop=%s(%u)=%llu%s\n", o.id, p->name,
				       p->prop_id, (unsigned long long)ps->prop_values[i],
				       imm ? " [IMM]" : "");
			n++;
		}
		if (p) drmModeFreeProperty(p);
	}
	drmModeFreeObjectProperties(ps);
	return n;
}

int main(void)
{
	int fd = open("/dev/dri/card1", O_RDWR | O_CLOEXEC);
	drmModeRes *res;
	drmModePlaneRes *pres;
	struct obj objs[64];
	int nobj = 0;

	if (fd < 0) { perror("open"); return 2; }
	drmSetClientCap(fd, DRM_CLIENT_CAP_ATOMIC, 1);
	res = drmModeGetResources(fd);
	pres = drmModeGetPlaneResources(fd);
	if (!res || !pres) return 3;
	for (int i = 0; i < res->count_connectors; i++)
		objs[nobj++] = (struct obj){ res->connectors[i], DRM_MODE_OBJECT_CONNECTOR };
	for (int i = 0; i < res->count_crtcs; i++)
		objs[nobj++] = (struct obj){ res->crtcs[i], DRM_MODE_OBJECT_CRTC };
	for (uint32_t i = 0; i < pres->count_planes; i++)
		objs[nobj++] = (struct obj){ pres->planes[i], DRM_MODE_OBJECT_PLANE };

	printf("== all props, all objects ==\n");
	drmModeAtomicReq *req = drmModeAtomicAlloc();
	for (int i = 0; i < nobj; i++) add_props(fd, req, objs[i], 0, 0);
	commit(fd, req, "ALL");
	drmModeAtomicFree(req);

	printf("== immutable only ==\n");
	req = drmModeAtomicAlloc();
	for (int i = 0; i < nobj; i++) add_props(fd, req, objs[i], 1, 1);
	commit(fd, req, "IMMUTABLE ONLY");
	drmModeAtomicFree(req);

	printf("== mutable only ==\n");
	req = drmModeAtomicAlloc();
	for (int i = 0; i < nobj; i++) add_props(fd, req, objs[i], 1, 0);
	commit(fd, req, "MUTABLE ONLY");
	drmModeAtomicFree(req);

	printf("== per object ==\n");
	for (int i = 0; i < nobj; i++) {
		char buf[64];
		req = drmModeAtomicAlloc();
		if (!add_props(fd, req, objs[i], 0, 0)) {
			drmModeAtomicFree(req);
			continue;
		}
		snprintf(buf, sizeof(buf), "obj %u type %u", objs[i].id, objs[i].type);
		commit(fd, req, buf);
		drmModeAtomicFree(req);
	}
	return 0;
}
