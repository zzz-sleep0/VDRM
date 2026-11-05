/*
 * vdrm_smoke.c — 内核模块基础功能冒烟（freestanding aarch64，无 libc）
 *
 * 用裸 syscall 直接打 /dev/vdrm、/dev/dri/card1、/dev/dma_heap/system、
 * /dev/vdrm-audio，验证：
 *   1. GET_INFO / GET_POOL 的 ABI 与空池
 *   2. REGISTER（真 dma-buf）→ GET_POOL 能原样转发 fd → UNREGISTER
 *   3. DRM CREATE_DUMB 从池里发缓冲；池空时 -ENOSPC；MAP_DUMB + mmap 成功
 *   4. 音频环 GET_INFO + mmap，环头字段正确
 *
 * 编译（本机）：
 *   clang --target=aarch64-linux-gnu -nostdlib -static -O1 -ffreestanding \
 *         -fno-builtin -fno-stack-protector -o vdrm_smoke vdrm_smoke.c
 * 运行：adb push vdrm_smoke /data/local/tmp/ && su -c /data/local/tmp/vdrm_smoke
 */

typedef unsigned short u16;
typedef unsigned int u32;
typedef int s32;
typedef unsigned long long u64;
typedef long i64;

#define SYS_openat 56
#define SYS_close 57
#define SYS_ioctl 29
#define SYS_mmap 222
#define SYS_munmap 215
#define SYS_exit_group 94

static inline long sc(long n, long a, long b, long c, long d, long e, long f)
{
	register long x8 __asm__("x8") = n;
	register long x0 __asm__("x0") = a;
	register long x1 __asm__("x1") = b;
	register long x2 __asm__("x2") = c;
	register long x3 __asm__("x3") = d;
	register long x4 __asm__("x4") = e;
	register long x5 __asm__("x5") = f;

	__asm__ volatile("svc #0"
			 : "+r"(x0)
			 : "r"(x8), "r"(x1), "r"(x2), "r"(x3), "r"(x4), "r"(x5)
			 : "memory");
	return x0;
}

static int sys_open(const char *p, int flags)
{
	return (int)sc(SYS_openat, -100 /* AT_FDCWD */, (long)p, flags, 0, 0, 0);
}

static void sys_close(int fd) { sc(SYS_close, fd, 0, 0, 0, 0, 0); }

static long sys_write(int fd, const void *buf, unsigned long n)
{
	return sc(64 /* write */, fd, (long)buf, n, 0, 0, 0);
}

static long sys_ioctl(int fd, unsigned long req, void *arg)
{
	return sc(SYS_ioctl, fd, req, (long)arg, 0, 0, 0);
}

static void *sys_mmap(void *addr, unsigned long len, int prot, int flags,
		      int fd, unsigned long off)
{
	return (void *)sc(SYS_mmap, (long)addr, len, prot, flags, fd, off);
}

static void sys_munmap(void *p, unsigned long len) { sc(SYS_munmap, (long)p, len, 0, 0, 0, 0); }

static void sys_exit(int code) { sc(SYS_exit_group, code, 0, 0, 0, 0, 0); }

/* ---- 输出 ---- */
static void *memset(void *d, int c, unsigned long n)
{
	unsigned char *p = d;
	while (n--)
		*p++ = (unsigned char)c;
	return d;
}

static void out(const char *s)
{
	unsigned long n = 0;

	while (s[n])
		n++;
	sys_write(1, s, n);
}

static void out_num(long v, int base)
{
	char buf[24];
	int i = 0, neg = 0;
	unsigned long u;

	if (v < 0 && base == 10) {
		neg = 1;
		u = (unsigned long)-v;
	} else {
		u = (unsigned long)v;
	}
	do {
		int d = u % base;

		buf[i++] = (char)(d < 10 ? '0' + d : 'a' + d - 10);
		u /= base;
	} while (u && i < (int)sizeof(buf) - 1);
	if (neg)
		buf[i++] = '-';
	while (i--)
		sys_write(1, &buf[i], 1);
}

static void out_hex(unsigned long v)
{
	out("0x");
	out_num((long)v, 16);
}

static int g_fail;
static int g_pass;

static void check(const char *name, int ok, long detail)
{
	out(ok ? "  PASS " : "  FAIL ");
	out(name);
	if (!ok) {
		out("  (detail=");
		out_num(detail, 10);
		out(")");
		g_fail++;
	} else {
		g_pass++;
	}
	out("\n");
}

/* ---- ABI（与 vdrm_ko.c / vdrm_audio.h 逐字节一致）---- */
#define VDRM_IOC(dir, type, nr, size) \
	((unsigned int)(((unsigned int)(dir) << 30) | ((unsigned int)(size) << 16) | \
			((unsigned int)(type) << 8) | (unsigned int)(nr)))

struct vdrm_info {
	u32 abi_version;
	u32 width, height, format, pitch;
	u32 pool_capacity;
	u32 flags;
	u32 reserved;
};

struct vdrm_register_req {
	s32 fd;
	s32 fd1;
	u32 width, height, format, pitch;
	u32 num_ints;
	u32 id;
	u32 ints[40];
	u64 user_data;
};

struct vdrm_handle_req {
	u32 id;
	u32 flags;
};

struct vdrm_pool_info {
	s32 fd;
	s32 fd1;
	u32 id, width, height, pitch, format;
	u32 num_ints;
	u32 ints[40];
	u32 flags;
};

#define VDRM_POOL_MAX 16
struct vdrm_pool_list {
	u32 count;
	u32 reserved;
	struct vdrm_pool_info items[VDRM_POOL_MAX];
};

#define VDRM_IOCTL_GET_INFO  VDRM_IOC(2, 'V', 0x00, sizeof(struct vdrm_info))
#define VDRM_IOCTL_REGISTER  VDRM_IOC(3, 'V', 0x01, sizeof(struct vdrm_register_req))
#define VDRM_IOCTL_RETURN    VDRM_IOC(1, 'V', 0x02, sizeof(struct vdrm_handle_req))
#define VDRM_IOCTL_UNREGISTER VDRM_IOC(1, 'V', 0x03, sizeof(struct vdrm_handle_req))
#define VDRM_IOCTL_INPUT_BATCH VDRM_IOC(1, 'V', 0x0b, 8 + 16 * 8)
#define VDRM_IOCTL_GET_POOL  VDRM_IOC(3, 'V', 0x07, sizeof(struct vdrm_pool_list))

struct vdrm_input_event {
	u16 type;
	u16 code;
	s32 value;
};

struct vdrm_audio_info {
	u32 magic;
	u32 size;
	u32 rate;
	u32 channels;
	u32 format;
	u32 header_size;
};

#define VDRM_AUDIO_GET_INFO 0x80187630u	/* _IOR('v', 0x30, 24) */

struct vdrm_audio_ring {
	u32 magic;
	u32 size;
	u32 head;
	u32 tail;
	u32 rate;
	u32 channels;
	u32 format;
	u32 generation;
};

/* DRM */
struct drm_mode_create_dumb {
	u32 height;
	u32 width;
	u32 bpp;
	u32 flags;
	u32 handle;
	u32 pitch;
	u64 size;
};

struct drm_mode_map_dumb {
	u32 handle;
	u32 pad;
	u64 offset;
};

#define DRM_IOCTL_MODE_CREATE_DUMB 0xc02064b2u	/* _IOWR('d', 0xB2, 32) */
#define DRM_IOCTL_MODE_MAP_DUMB	   0xc01064b3u	/* _IOWR('d', 0xB3, 16) */
#define DRM_IOCTL_PRIME_FD_TO_HANDLE 0xc00c642eu	/* _IOWR('d', 0x2E, 12) */
#define DRM_IOCTL_GEM_CLOSE	   0x40086409u	/* _IOW('d', 0x09, 8) */

struct drm_prime_handle {
	u32 handle;
	u32 flags;
	s32 fd;
};

struct drm_gem_close {
	u32 handle;
	u32 pad;
};

/* dma-heap */
struct dma_heap_allocation_data {
	u64 len;
	u32 fd;
	u32 fd_flags;
	u64 heap_flags;
};

#define DMA_HEAP_IOCTL_ALLOC 0xc0184800u	/* _IOWR('H', 0, 24) */

#define O_RDWR 2
#define PROT_READ 1
#define PROT_WRITE 2
#define MAP_SHARED 1
#define ENOSPC 28
#define EINVAL 22

static struct vdrm_pool_list g_list;

static void dump_pool(int n)
{
	int i;

	out("  pool entries = ");
	out_num(n, 10);
	out("\n");
	for (i = 0; i < n && i < VDRM_POOL_MAX; i++) {
		out("    [");
		out_num(i, 10);
		out("] id=");
		out_num(g_list.items[i].id, 10);
		out(" fd=");
		out_num(g_list.items[i].fd, 10);
		out(" fd1=");
		out_num(g_list.items[i].fd1, 10);
		out(" ");
		out_num(g_list.items[i].width, 10);
		out("x");
		out_num(g_list.items[i].height, 10);
		out(" pitch=");
		out_num(g_list.items[i].pitch, 10);
		out("\n");
	}
}

static int run(void)
{
	struct vdrm_info info;
	struct vdrm_register_req reg;
	struct vdrm_handle_req hreq;
	struct vdrm_input_event ev[2];
	struct vdrm_audio_info ainfo;
	struct vdrm_audio_ring *ring;
	struct drm_mode_create_dumb dumb;
	struct drm_mode_map_dumb map;
	struct dma_heap_allocation_data alloc;
	u32 mode_w, mode_h;
	long rc;
	int fd, drm, heap, audio;

	out("== vdrm kernel smoke ==\n");

	fd = sys_open("/dev/vdrm", O_RDWR);
	check("open /dev/vdrm", fd >= 0, fd);
	if (fd < 0)
		return 1;

	/* 1) GET_INFO */
	info.width = info.height = info.pitch = info.abi_version = 0;
	rc = sys_ioctl(fd, VDRM_IOCTL_GET_INFO, &info);
	out("  GET_INFO rc=");
	out_num(rc, 10);
	out(" abi=");
	out_num(info.abi_version, 10);
	out(" mode=");
	out_num(info.width, 10);
	out("x");
	out_num(info.height, 10);
	out(" pitch=");
	out_num(info.pitch, 10);
	out(" cap=");
	out_num(info.pool_capacity, 10);
	out("\n");
	check("GET_INFO ok", rc == 0 && info.width > 0 && info.height > 0 &&
				     info.pool_capacity == VDRM_POOL_MAX,
	      rc);
	mode_w = info.width;
	mode_h = info.height;

	/* 2) 空池 */
	rc = sys_ioctl(fd, VDRM_IOCTL_GET_POOL, &g_list);
	out("  GET_POOL(empty) rc=");
	out_num(rc, 10);
	out(" count=");
	out_num((long)g_list.count, 10);
	out("\n");
	check("GET_POOL empty", rc == 0 && g_list.count == 0, g_list.count);

	/* 3) 用 dma-heap 造一块真 dma-buf 注册进池 */
	heap = sys_open("/dev/dma_heap/system", O_RDWR);
	check("open /dev/dma_heap/system", heap >= 0, heap);
	alloc.len = (u64)mode_w * mode_h * 4;
	alloc.fd = 0;			/* 入口必须为 0，非 0 直接 -EINVAL */
	alloc.fd_flags = 0x80000;	/* O_CLOEXEC */
	alloc.heap_flags = 0;
	if (heap >= 0) {
		rc = sys_ioctl(heap, DMA_HEAP_IOCTL_ALLOC, &alloc);
		out("  DMA_HEAP_ALLOC rc=");
		out_num(rc, 10);
		out(" fd=");
		out_num((long)alloc.fd, 10);
		out(" len=");
		out_num((long)alloc.len, 10);
		out("\n");
		check("dma-heap alloc", rc == 0 && alloc.fd != 0, rc);
	}
	if (heap >= 0 && rc == 0 && alloc.fd != 0) {
		reg.fd = (s32)alloc.fd;
		reg.fd1 = -1;
		reg.width = mode_w;
		reg.height = mode_h;
		reg.format = 5;		/* HAL BGRA_8888 */
		reg.pitch = mode_w * 4;
		reg.num_ints = 0;
		reg.id = 0;
		reg.user_data = 0x1234;
		rc = sys_ioctl(fd, VDRM_IOCTL_REGISTER, &reg);
		out("  REGISTER rc=");
		out_num(rc, 10);
		out(" id=");
		out_num((long)reg.id, 10);
		out("\n");
		check("REGISTER dma-buf", rc == 0 && reg.id > 0, rc);

		/* 4) GET_POOL 应能原样转发 fd */
		rc = sys_ioctl(fd, VDRM_IOCTL_GET_POOL, &g_list);
		dump_pool((int)g_list.count);
		check("GET_POOL has 1", rc == 0 && g_list.count == 1, g_list.count);
		if (g_list.count == 1) {
			check("pool entry fd forwarded", g_list.items[0].fd >= 0,
			      g_list.items[0].fd);
			check("pool entry fd1 = -1", g_list.items[0].fd1 == -1,
			      g_list.items[0].fd1);
			check("pool entry geom",
			      g_list.items[0].width == mode_w &&
				      g_list.items[0].height == mode_h,
			      (long)g_list.items[0].width);
			sys_close(g_list.items[0].fd);
			if (g_list.items[0].fd1 >= 0)
				sys_close(g_list.items[0].fd1);
		}

		/* 4b) render 节点（牌子）：DRM core 里 CREATE_DUMB/MAP_DUMB 本来就带
		 * DRM_RENDER_ALLOW，所以保留牌子的同时它也能发池缓冲；PRIME 也必须
		 * 可用。这里验证它走的是同一个池（占用会让 UNREGISTER 变 EBUSY）。*/
		{
			int rfd = sys_open("/dev/dri/renderD129", O_RDWR);

			if (rfd >= 0) {
				struct drm_mode_create_dumb d3;
				struct drm_prime_handle ph;
				struct drm_gem_close gc;
				struct vdrm_handle_req h2;

				d3.height = mode_h;
				d3.width = mode_w;
				d3.bpp = 32;
				d3.flags = 0;
				d3.handle = 0;
				d3.pitch = 0;
				d3.size = 0;
				rc = sys_ioctl(rfd, DRM_IOCTL_MODE_CREATE_DUMB, &d3);
				check("render: CREATE_DUMB ok",
				      rc == 0 && d3.handle > 0 && d3.size > 0, rc);

				h2.id = reg.id;
				h2.flags = 0;
				rc = sys_ioctl(fd, VDRM_IOCTL_UNREGISTER, &h2);
				check("render: slot reserved -> UNREGISTER EBUSY",
				      rc == -16, rc);

				memset(&ph, 0, sizeof(ph));
				ph.fd = (s32)alloc.fd;
				rc = sys_ioctl(rfd, DRM_IOCTL_PRIME_FD_TO_HANDLE, &ph);
				check("render: PRIME import ok",
				      rc == 0 && ph.handle > 0, rc);
				if (rc == 0) {
					gc.handle = ph.handle;
					gc.pad = 0;
					rc = sys_ioctl(rfd, DRM_IOCTL_GEM_CLOSE, &gc);
					check("render: GEM_CLOSE ok", rc == 0, rc);
				}
				/* 关掉 render fd -> postclose 把占用还回池 */
				sys_close(rfd);
				rc = sys_ioctl(fd, VDRM_IOCTL_GET_POOL, &g_list);
				check("render: postclose released slot",
				      rc == 0 && g_list.count == 1, (long)g_list.count);
			} else {
				out("  (no /dev/dri/renderD129, skip render-node checks)\n");
			}
		}

		/* 5) DRM 侧：从池里发 dumb buffer，再 map 一次 */
		drm = sys_open("/dev/dri/card1", O_RDWR);
		check("open /dev/dri/card1", drm >= 0, drm);
		if (drm >= 0) {
			dumb.height = mode_h;
			dumb.width = mode_w;
			dumb.bpp = 32;
			dumb.flags = 0;
			dumb.handle = 0;
			dumb.pitch = 0;
			dumb.size = 0;
			rc = sys_ioctl(drm, DRM_IOCTL_MODE_CREATE_DUMB, &dumb);
			out("  CREATE_DUMB rc=");
			out_num(rc, 10);
			out(" handle=");
			out_num((long)dumb.handle, 10);
			out(" pitch=");
			out_num((long)dumb.pitch, 10);
			out(" size=");
			out_num((long)dumb.size, 10);
			out("\n");
			check("CREATE_DUMB from pool",
			      rc == 0 && dumb.handle > 0 && dumb.size > 0, rc);

			if (rc == 0) {
				map.handle = dumb.handle;
				map.pad = 0;
				map.offset = 0;
				rc = sys_ioctl(drm, DRM_IOCTL_MODE_MAP_DUMB, &map);
				check("MAP_DUMB", rc == 0 && map.offset > 0, rc);
				if (rc == 0) {
					void *p = sys_mmap(0, (unsigned long)dumb.size,
							   PROT_READ | PROT_WRITE,
							   MAP_SHARED, drm, map.offset);

					check("mmap pool buffer",
					      (long)p > 0 && (long)p > -4096, (long)p);
					if ((long)p > 0) {
						volatile u32 *w = (volatile u32 *)p;

						w[0] = 0x11223344;
						check("mmap write/read",
						      w[0] == 0x11223344,
						      (long)w[0]);
						sys_munmap(p, (unsigned long)dumb.size);
					}
				}

				/* 池里只有一块 -> 第二次必须 -ENOSPC */
				{
					struct drm_mode_create_dumb d2 = dumb;

					d2.handle = 0;
					rc = sys_ioctl(drm, DRM_IOCTL_MODE_CREATE_DUMB, &d2);
					check("second CREATE_DUMB = ENOSPC",
					      rc == -ENOSPC, rc);
				}
			}
			/* 槽还被合成器占着（allocation_reserved）时，UNREGISTER 必须拒绝 */
			hreq.id = reg.id;
			hreq.flags = 0;
			rc = sys_ioctl(fd, VDRM_IOCTL_UNREGISTER, &hreq);
			check("UNREGISTER while reserved = EBUSY", rc == -16 /* EBUSY */, rc);

			/* 关掉 drm fd -> postclose 归还占用 */
			sys_close(drm);
		}

		/* 6) UNREGISTER（postclose 之后槽已 FREE） */
		hreq.id = reg.id;
		hreq.flags = 0;
		rc = sys_ioctl(fd, VDRM_IOCTL_UNREGISTER, &hreq);
		check("UNREGISTER id", rc == 0, rc);

		/* 池应回到空 */
		rc = sys_ioctl(fd, VDRM_IOCTL_GET_POOL, &g_list);
		check("GET_POOL empty after unregister",
		      rc == 0 && g_list.count == 0, (long)g_list.count);
		sys_close((int)alloc.fd);
	}
	if (heap >= 0)
		sys_close(heap);

	/* 7) 输入批量：只发 EV_SYN（type 0 -> 无设备，纯 no-op 路径） */
	ev[0].type = 0;
	ev[0].code = 0;
	ev[0].value = 0;
	{
		unsigned char batch[8 + 16 * 8] = { 0 };

		/* count=1, flags=0, 然后一条事件 */
		*(u32 *)&batch[0] = 1;
		*(u16 *)&batch[8] = ev[0].type;
		*(u16 *)&batch[10] = ev[0].code;
		*(s32 *)&batch[12] = ev[0].value;
		rc = sys_ioctl(fd, VDRM_IOCTL_INPUT_BATCH, batch);
		check("INPUT_BATCH no-op", rc == 0, rc);
	}

	/* 8) 音频环 */
	audio = sys_open("/dev/vdrm-audio", O_RDWR);
	check("open /dev/vdrm-audio", audio >= 0, audio);
	if (audio >= 0) {
		rc = sys_ioctl(audio, VDRM_AUDIO_GET_INFO, &ainfo);
		out("  AUDIO_GET_INFO rc=");
		out_num(rc, 10);
		out(" size=");
		out_num((long)ainfo.size, 10);
		out(" rate=");
		out_num((long)ainfo.rate, 10);
		out(" ch=");
		out_num((long)ainfo.channels, 10);
		out(" hdr=");
		out_num((long)ainfo.header_size, 10);
		out("\n");
		check("audio GET_INFO",
		      rc == 0 && ainfo.size == 512 * 1024 && ainfo.rate == 48000 &&
			      ainfo.channels == 2 && ainfo.header_size == 32,
		      rc);
		if (rc == 0) {
			ring = sys_mmap(0, 4096 + ainfo.size,
					PROT_READ | PROT_WRITE, MAP_SHARED, audio, 0);
			check("mmap audio ring", (long)ring > 0 && (long)ring > -4096,
			      (long)ring);
			if ((long)ring > 0) {
				out("  ring magic=");
				out_hex(ring->magic);
				out(" size=");
				out_num((long)ring->size, 10);
				out(" gen=");
				out_num((long)ring->generation, 10);
				out("\n");
				check("ring header sane",
				      ring->magic == 0x56445241u &&
					      ring->size == 512 * 1024,
				      (long)ring->magic);
				sys_munmap(ring, 4096 + ainfo.size);
			}
		}
		sys_close(audio);
	}

	sys_close(fd);

	out("\n== result: ");
	out_num(g_pass, 10);
	out(" pass, ");
	out_num(g_fail, 10);
	out(" fail ==\n");
	return g_fail ? 1 : 0;
}

void _start(void)
{
	sys_exit(run());
}
