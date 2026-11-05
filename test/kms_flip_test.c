/*
 * kms_flip_test.c — 容器侧原生测试：模拟合成器走完"池 → dumb → ADDFB2 →
 * SetCrtc → PAGE_FLIP → 翻转事件 → App 收到帧事件 → RETURN"整条链路。
 *
 * 依赖：/dev/vdrm、/dev/dri/card1、/dev/dma_heap/system 已绑进容器。
 * 编译：gcc -O1 -o kms_flip_test kms_flip_test.c -ldrm
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/mman.h>

#include <xf86drm.h>
#include <xf86drmMode.h>
#include <drm.h>
#include <drm_fourcc.h>

/* ---- /dev/vdrm ABI（与 vdrm_ko.c 一致）---- */
#define VDRM_IOC(dir, type, nr, size) \
	((unsigned int)(((unsigned int)(dir) << 30) | ((unsigned int)(size) << 16) | \
			((unsigned int)(type) << 8) | (unsigned int)(nr)))

struct vdrm_info {
	uint32_t abi_version, width, height, format, pitch, pool_capacity, flags, reserved;
};
struct vdrm_register_req {
	int32_t fd, fd1;
	uint32_t width, height, format, pitch, num_ints, id;
	uint32_t ints[40];
	uint64_t user_data;
};
struct vdrm_handle_req { uint32_t id, flags; };
struct vdrm_frame_event { uint32_t id, sequence; uint64_t timestamp_ns, user_data; };

#define VDRM_IOCTL_GET_INFO   VDRM_IOC(2, 'V', 0x00, sizeof(struct vdrm_info))
#define VDRM_IOCTL_REGISTER   VDRM_IOC(3, 'V', 0x01, sizeof(struct vdrm_register_req))
#define VDRM_IOCTL_RETURN     VDRM_IOC(1, 'V', 0x02, sizeof(struct vdrm_handle_req))
#define VDRM_IOCTL_UNREGISTER VDRM_IOC(1, 'V', 0x03, sizeof(struct vdrm_handle_req))

/* ---- dma-heap ---- */
struct dma_heap_allocation_data {
	uint64_t len;
	uint32_t fd, fd_flags;
	uint64_t heap_flags;
};
#define DMA_HEAP_IOCTL_ALLOC _IOWR('H', 0x0, struct dma_heap_allocation_data)

static int g_fail;
static void ok(const char *name, int cond, long detail)
{
	printf("  %s %s", cond ? "PASS" : "FAIL", name);
	if (!cond) printf(" (detail=%ld)", detail);
	printf("\n");
	if (!cond) g_fail++;
}

int main(void)
{
	struct vdrm_info info;
	struct vdrm_register_req reg;
	struct vdrm_handle_req hreq;
	struct vdrm_frame_event ev;
	struct dma_heap_allocation_data al;
	int vfd, hfd, cfd, i, rc;
	uint32_t handle = 0, pitch = 0, fb = 0;
	uint64_t size = 0;
	uint32_t *map = NULL;
	drmModeRes *res = NULL;
	drmModeConnector *conn = NULL;
	drmModeCrtc *crtc = NULL;
	drmModeModeInfo mode;

	printf("== vdrm KMS flip test ==\n");

	vfd = open("/dev/vdrm", O_RDWR | O_CLOEXEC);
	ok("open /dev/vdrm", vfd >= 0, errno);
	if (vfd < 0) return 1;
	if (ioctl(vfd, VDRM_IOCTL_GET_INFO, &info)) { perror("GET_INFO"); return 1; }
	printf("  mode=%ux%u pitch=%u cap=%u\n", info.width, info.height, info.pitch,
	       info.pool_capacity);

	hfd = open("/dev/dma_heap/system", O_RDWR | O_CLOEXEC);
	ok("open /dev/dma_heap/system", hfd >= 0, errno);
	if (hfd < 0) return 1;
	memset(&al, 0, sizeof(al));
	al.len = (uint64_t)info.width * info.height * 4;
	al.fd = 0;
	al.fd_flags = O_CLOEXEC;
	ok("dma-heap alloc", ioctl(hfd, DMA_HEAP_IOCTL_ALLOC, &al) == 0, errno);

	/* 池是模块级的、跨进程存活（设计如此）：先像 mkpool 一样清一次，
	 * 否则 CREATE_DUMB 可能发到上一轮遗留的槽，帧事件里的 id 就不是本次
	 * 注册的那个。*/
	{
		struct vdrm_handle_req clr = { .id = 0, .flags = 0 };
		ioctl(vfd, VDRM_IOCTL_UNREGISTER, &clr);
	}

	memset(&reg, 0, sizeof(reg));
	reg.fd = al.fd;
	reg.fd1 = -1;
	reg.width = info.width;
	reg.height = info.height;
	reg.format = 5;			/* HAL BGRA_8888 */
	reg.pitch = info.width * 4;
	ok("REGISTER", ioctl(vfd, VDRM_IOCTL_REGISTER, &reg) == 0 && reg.id > 0, errno);
	printf("  pool id=%u\n", reg.id);

	cfd = open("/dev/dri/card1", O_RDWR | O_CLOEXEC);
	ok("open /dev/dri/card1", cfd >= 0, errno);
	if (cfd < 0) return 1;
	res = drmModeGetResources(cfd);
	ok("drmModeGetResources", res != NULL, errno);
	if (!res) return 1;
	printf("  counts: crtc=%d conn=%d enc=%d\n", res->count_crtcs, res->count_connectors,
	       res->count_encoders);

	for (i = 0; i < res->count_connectors; i++) {
		drmModeConnector *c = drmModeGetConnector(cfd, res->connectors[i]);
		if (c && c->count_modes > 0) { conn = c; break; }
		if (c) drmModeFreeConnector(c);
	}
	ok("connector with modes", conn != NULL, 0);
	if (!conn) return 1;
	mode = conn->modes[0];
	printf("  connector %u type=%u mode=%s %ux%u@%u\n", conn->connector_id,
	       conn->connector_type, mode.name, mode.hdisplay, mode.vdisplay,
	       mode.vrefresh);

	crtc = drmModeGetCrtc(cfd, res->crtcs[0]);
	ok("drmModeGetCrtc", crtc != NULL, errno);

	rc = drmModeCreateDumbBuffer(cfd, info.width, info.height, 32, 0, &handle, &pitch,
				     &size);
	ok("CREATE_DUMB (from pool)", rc == 0 && handle != 0 && size > 0, errno);
	printf("  dumb handle=%u pitch=%u size=%llu\n", handle, pitch, (unsigned long long)size);
	if (rc != 0) return 1;

	{
		uint64_t off = 0;
		rc = drmModeMapDumbBuffer(cfd, handle, &off);
		ok("MAP_DUMB", rc == 0, errno);
		if (rc == 0) {
			map = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, cfd, off);
			ok("mmap dumb", map != MAP_FAILED, (long)map);
			if (map != MAP_FAILED) {
				for (uint32_t k = 0; k < info.width * info.height; k++)
					map[k] = 0xff336699;	/* BGRA */
			}
		}
	}

	rc = drmModeAddFB(cfd, info.width, info.height, 24, 32, pitch, handle, &fb);
	ok("ADDFB2", rc == 0 && fb != 0, errno);
	if (rc != 0) return 1;

	rc = drmModeSetCrtc(cfd, crtc->crtc_id, fb, 0, 0, &conn->connector_id, 1, &mode);
	ok("SetCrtc (modeset)", rc == 0, errno);

	/* SetCrtc 也是一次 commit -> 驱动应当把"槽已上屏"的帧事件推给 /dev/vdrm */
	{
		struct pollfd p = { .fd = vfd, .events = POLLIN };
		rc = poll(&p, 1, 2000);
		ok("frame event from /dev/vdrm", rc > 0 && (p.revents & POLLIN), rc);
		if (rc > 0) {
			ssize_t n = read(vfd, &ev, sizeof(ev));
			ok("read frame event", n == (ssize_t)sizeof(ev), n);
			printf("  event id=%u seq=%u\n", ev.id, ev.sequence);
			ok("event id == pool id", ev.id == reg.id, ev.id);
		}
	}

	/* PAGE_FLIP + 等 flip 完成事件 */
	rc = drmModePageFlip(cfd, crtc->crtc_id, fb, DRM_MODE_PAGE_FLIP_EVENT, NULL);
	ok("PAGE_FLIP armed", rc == 0, errno);
	{
		struct pollfd p = { .fd = cfd, .events = POLLIN };
		rc = poll(&p, 1, 2000);
		ok("flip-done event", rc > 0, rc);
		if (rc > 0) {
			drmEventContext ec = { .version = DRM_EVENT_CONTEXT_VERSION };
			drmHandleEvent(cfd, &ec);
		}
	}

	/* 第二次 poll：flip 之后 pipe_update 又推了一次帧事件 */
	{
		struct pollfd p = { .fd = vfd, .events = POLLIN };
		rc = poll(&p, 1, 2000);
		ok("frame event after flip", rc > 0, rc);
		if (rc > 0) read(vfd, &ev, sizeof(ev));
	}

	/* App 归还 */
	hreq.id = reg.id;
	hreq.flags = 0;
	rc = ioctl(vfd, VDRM_IOCTL_RETURN, &hreq);
	ok("RETURN", rc == 0, errno);

	/* 合成器侧收尾：rmfb/关 fd -> postclose 归还池槽的"合成器占用" */
	if (map && map != MAP_FAILED) munmap(map, size);
	if (fb) drmModeRmFB(cfd, fb);
	if (handle) drmModeDestroyDumbBuffer(cfd, handle);
	if (crtc) drmModeFreeCrtc(crtc);
	if (conn) drmModeFreeConnector(conn);
	if (res) drmModeFreeResources(res);
	close(cfd);

	/* 现在槽才真正回到 FREE，UNREGISTER 才允许 */
	hreq.id = reg.id;
	hreq.flags = 0;
	rc = ioctl(vfd, VDRM_IOCTL_UNREGISTER, &hreq);
	ok("UNREGISTER after postclose", rc == 0, errno);
	close(hfd);
	close(vfd);

	printf("== result: %d fail ==\n", g_fail);
	return g_fail ? 1 : 0;
}
