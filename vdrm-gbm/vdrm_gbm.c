/*
 * vdrm_gbm.so — 让 GBM 在 vdrm(card1) 上走 DRM dumb 分配路径
 *
 * 背景：libgbm 的 _gbm_create_device() 会按 DRM 驱动名找后端
 *   <drivername>_gbm.so（找不到才退回 dri_gbm.so）。
 * card1 的驱动名是 "vdrm"，所以只要把本 .so 放进 /usr/lib/gbm/，
 * 任何合成器在 card1 上调用 gbm_create_device() 都会自动加载它。
 *
 * 策略：把一切委托给标准 dri_gbm.so，只就地覆盖 3 个 vtable 函数：
 *   bo_create      -> 追加 GBM_BO_USE_WRITE，命中 dri 后端里已有的
 *                     create_dumb() 分支
 *                     => DRM_IOCTL_MODE_CREATE_DUMB -> vdrm 的 dumb_create（池）
 *   bo_get_fd      -> dumb BO 真函数返回 -1，这里用 PRIME 导出
 *   bo_get_plane_fd-> 同上
 *
 * 编译: gcc -shared -fPIC -O2 -Wall -o vdrm_gbm.so vdrm_gbm.c
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <fcntl.h>
#include <sys/ioctl.h>

#include "gbm_backend_abi.h"
#include "gbm.h"

/* 裸 PRIME_HANDLE_TO_FD，避免依赖 libdrm 头 */
struct vdrm_prime_handle {
	uint32_t handle;
	uint32_t flags;
	int32_t  fd;
	uint32_t pad;
};
#define VDRM_IOCTL_PRIME_HANDLE_TO_FD _IOWR('d', 0x2d, struct vdrm_prime_handle)

static const struct gbm_backend *dri_backend;
static struct gbm_device *(*dri_create_device)(int, uint32_t);

static struct gbm_bo *(*orig_bo_create)(struct gbm_device *, uint32_t, uint32_t,
					uint32_t, uint32_t, const uint64_t *,
					unsigned int);
static int (*orig_bo_get_fd)(struct gbm_bo *);
static int (*orig_bo_get_plane_fd)(struct gbm_bo *, int);

static struct gbm_bo *
vdrm_bo_create(struct gbm_device *gbm, uint32_t width, uint32_t height,
	       uint32_t format, uint32_t usage,
	       const uint64_t *modifiers, unsigned int count)
{
	/* dri_gbm.c:
	 *   if (usage & GBM_BO_USE_WRITE || !dri->has_dmabuf_export)
	 *           return create_dumb(...);
	 * 加 WRITE 即可强制走 CREATE_DUMB。 */
	return orig_bo_create(gbm, width, height, format,
			      usage | GBM_BO_USE_WRITE, modifiers, count);
}

static int prime_fd_from_dumb_bo(struct gbm_bo *bo)
{
	struct vdrm_prime_handle h;

	memset(&h, 0, sizeof(h));
	h.handle = bo->v0.handle.u32;      /* dumb BO 的 GEM handle */
	h.flags  = O_CLOEXEC | O_RDWR;     /* DRM_CLOEXEC | DRM_RDWR */
	h.fd     = -1;

	if (ioctl(bo->gbm->v0.fd, VDRM_IOCTL_PRIME_HANDLE_TO_FD, &h) != 0)
		return -1;
	return h.fd;
}

static int vdrm_bo_get_fd(struct gbm_bo *bo)
{
	int fd = orig_bo_get_fd ? orig_bo_get_fd(bo) : -1;

	if (fd >= 0)
		return fd;
	return prime_fd_from_dumb_bo(bo);
}

static int vdrm_bo_get_plane_fd(struct gbm_bo *bo, int plane)
{
	int fd = orig_bo_get_plane_fd ? orig_bo_get_plane_fd(bo, plane) : -1;

	if (fd >= 0)
		return fd;
	if (plane != 0)
		return -1;
	return prime_fd_from_dumb_bo(bo);
}

static struct gbm_device *vdrm_create_device(int fd, uint32_t abi_ver)
{
	struct gbm_device *dev;

	if (!dri_create_device)
		return NULL;

	dev = dri_create_device(fd, abi_ver);   /* 真 dri 设备 */
	if (!dev)
		return NULL;

	/* 就地覆盖 vtable（dev 就是真实的 dri 设备，base 在偏移 0） */
	orig_bo_create       = dev->v0.bo_create;
	orig_bo_get_fd       = dev->v0.bo_get_fd;
	orig_bo_get_plane_fd = dev->v0.bo_get_plane_fd;

	dev->v0.bo_create = vdrm_bo_create;
	dev->v0.bo_get_fd = vdrm_bo_get_fd;
	if (orig_bo_get_plane_fd)
		dev->v0.bo_get_plane_fd = vdrm_bo_get_plane_fd;

	return dev;
}

static const struct gbm_backend vdrm_backend = {
	.v0 = {
		.backend_version = GBM_BACKEND_ABI_VERSION,
		.backend_name    = "vdrm",
		.create_device   = vdrm_create_device,
	},
};

/*
 * 定位 dri_gbm.so。
 *
 * 不再写死 /usr/lib/gbm/ —— Debian/Ubuntu 是 /usr/lib/<triplet>/gbm/，
 * 而且 Mesa 还认 GBM_BACKENDS_PATH。写死只对 Arch 系成立。
 *
 * 主路径用 dladdr 从【我们自己】的加载位置推：libgbm 是从某个后端目录里
 * 找到我们的，而 <驱动名>_gbm.so 和 dri_gbm.so 是同一套查找规则下的东西，
 * 所以"我们在哪"就等于"它在哪"。不需要任何环境变量或发行版知识。
 * 下面两个是兜底，正常情况下走不到。
 */
static void *vdrm_open_dri_backend(void)
{
	Dl_info info;
	char path[4096];

	if (dladdr((void *)&vdrm_create_device, &info) && info.dli_fname) {
		const char *slash = strrchr(info.dli_fname, '/');
		if (slash) {
			size_t n = (size_t)(slash - info.dli_fname);
			if (n + sizeof("/dri_gbm.so") < sizeof(path)) {
				void *lib;
				memcpy(path, info.dli_fname, n);
				memcpy(path + n, "/dri_gbm.so", sizeof("/dri_gbm.so"));
				lib = dlopen(path, RTLD_NOW | RTLD_LOCAL);
				if (lib)
					return lib;
			}
		}
	}

	{
		static const char *cand[] = {
			"/usr/lib/gbm/dri_gbm.so",
			"/usr/lib/aarch64-linux-gnu/gbm/dri_gbm.so",
			"/usr/lib64/gbm/dri_gbm.so",
			"/usr/local/lib/gbm/dri_gbm.so",
			NULL
		};
		int i;
		for (i = 0; cand[i]; i++) {
			void *lib = dlopen(cand[i], RTLD_NOW | RTLD_LOCAL);
			if (lib)
				return lib;
		}
	}

	return dlopen("dri_gbm.so", RTLD_NOW | RTLD_LOCAL);
}

const struct gbm_backend *gbmint_get_backend(const struct gbm_core *core)
{
	if (!dri_backend) {
		void *lib = vdrm_open_dri_backend();
		if (lib) {
			GBM_GET_BACKEND_PROC_PTR get =
				(GBM_GET_BACKEND_PROC_PTR)dlsym(lib,
					GBM_GET_BACKEND_PROC_NAME);
			if (get)
				dri_backend = get(core);
		}
	}
	if (dri_backend)
		dri_create_device = dri_backend->v0.create_device;

	return &vdrm_backend;
}
