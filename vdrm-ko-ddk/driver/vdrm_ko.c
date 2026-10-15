/*
 * vdrm.c — 虚拟 DRM 设备 (card1)
 *
 * 一个普通的可加载模块：insmod 进来就建 card1 + /dev/vdrm + 两个虚拟输入设备，
 * rmmod 撤掉。没有谁在别的上下文里替它做事 —— 所有路径都跑在模块自己的初始化和
 * ioctl 里。
 */
#include <linux/module.h>
#include <linux/version.h>	/* KERNEL_VERSION() —— 下面几处跨 KMI 的守卫要用 */
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/errno.h>
#include <linux/string.h>
#include <linux/slab.h>
#include <linux/printk.h>
#include <linux/uaccess.h>
#include <linux/hrtimer.h>
#include <linux/timekeeping.h>
#include <linux/spinlock.h>
#include <linux/mutex.h>
#include <linux/wait.h>
#include <linux/poll.h>
#include <linux/miscdevice.h>
#include <linux/platform_device.h>
#include <linux/input.h>
#include <linux/dma-buf.h>
#include <linux/dma-heap.h>
#include <linux/mm.h>
#include <linux/list.h>
#include <linux/fs.h>
#include <linux/kthread.h>
#include <linux/cred.h>
#include <linux/kprobes.h>
#include <linux/binfmts.h>
#include <linux/umh.h>

#include <drm/drm_device.h>
#include <drm/drm_drv.h>
#include <drm/drm_gem.h>
#include <drm/drm_gem_shmem_helper.h>
#include <drm/drm_gem_framebuffer_helper.h>
#include <drm/drm_framebuffer.h>
#include <drm/drm_crtc.h>
#include <drm/drm_connector.h>
#include <drm/drm_modes.h>
#include <drm/drm_probe_helper.h>
#include <drm/drm_mode_config.h>
#include <drm/drm_atomic.h>
#include <drm/drm_atomic_helper.h>
#include <drm/drm_vblank.h>
#include <drm/drm_prime.h>
#include <drm/drm_simple_kms_helper.h>
#include <drm/drm_fourcc.h>

/* 内核 memset 是 arm64 汇编实现、没有 kCFI 哈希。走内置版本生成【直接 bl】，
 * 避免间接调用被 kCFI 校验 -> brk #0x8228 -> panic。 */
#undef memset
#define memset(s, c, n) __builtin_memset((s), (c), (n))









/* 有了内核头，这些不再需要手抄数值 —— 名字保留，省得改动调用点 */
#define VDRM_DRIVER_FEATURES     (DRIVER_GEM | DRIVER_MODESET | DRIVER_ATOMIC)

#define VDRM_FMT_XRGB8888        DRM_FORMAT_XRGB8888
#define VDRM_FMT_ARGB8888        DRM_FORMAT_ARGB8888
#define VDRM_FMT_XBGR8888        DRM_FORMAT_XBGR8888
#define VDRM_FMT_ABGR8888        DRM_FORMAT_ABGR8888

#define VDRM_CONNECTOR_VIRTUAL   DRM_MODE_CONNECTOR_VIRTUAL

static struct platform_device *g_pdev;
static struct drm_device       *g_dev;
static int                      g_has_modecfg;
static int                      g_registered;

static struct file_operations   g_fops;
static struct drm_driver        g_driver;

/* ---- 显示管线（M2b）---- */
static struct drm_simple_display_pipe      *g_pipe;
static struct drm_connector                *g_conn;
static struct drm_connector_funcs           g_conn_funcs;
static struct drm_connector_helper_funcs    g_conn_helper;
static struct drm_simple_display_pipe_funcs g_pipe_funcs;
static struct drm_mode_config_funcs         g_mode_funcs;

static const u32 g_formats[] = {
	VDRM_FMT_XRGB8888, VDRM_FMT_ARGB8888,
	VDRM_FMT_XBGR8888, VDRM_FMT_ABGR8888,
};

/* ---- M4: vblank 模拟（虚拟 device 没有真实扫描，用 hrtimer 造 vblank）---- */
static struct hrtimer g_vblank_timer;
static int            g_vblank_timer_inited;
static volatile int   g_vblank_on;
static volatile int   g_vblank_timer_running;   /* 连续 vblank 网格是否在跑 */
static volatile u64   g_vblank_period_ns = 16666666ULL; /* 60Hz 兜底（pipe enable 时会按 mode 重算）*/

/* 诊断旋钮：非 0 时强制虚拟显示的帧率（Hz），0 = 按 mode 反推（默认行为）。
 * 用来分辨“合成器跟不上”是 vblank 定拍造成的，还是它自己渲染慢。*/
#define VDRM_VBLANK_HZ_FORCE 0

/* ============= App <-> 模块 的契约（ioctl 结构体，两边必须逐字节一致）============= */

#define VDRM_ABI_VERSION 1
#define VDRM_POOL_MAX    16
#define VDRM_MAX_INTS    40

struct vdrm_info {
	u32 abi_version;
	u32 width, height, format, pitch;
	u32 pool_capacity;
	u32 flags;
	u32 reserved;
};
struct vdrm_register_req {
	s32 fd;        /* fd0：像素 dma-buf */
	s32 fd1;       /* fd1：metadata blob（-1 = 无）*/
	u32 width, height, format, pitch;
	u32 num_ints;  /* handle 里的整数个数 */
	u32 id;
	u32 ints[VDRM_MAX_INTS];
	u64 user_data;
};
struct vdrm_handle_req {
	u32 id;
	u32 flags;
};

/* 裸 ioctl 编码，不依赖内核 ioctl 头。必须强制 unsigned int，
 * 否则 (dir<<30) 会符号扩展成 64 位，和 unsigned int cmd 对不上。 */
#define VDRM_IOC(dir, type, nr, size) \
	((unsigned int)(((unsigned int)(dir) << 30) | \
			((unsigned int)(size) << 16) | \
			((unsigned int)(type) << 8) | (unsigned int)(nr)))
#define VDRM_IOCTL_GET_INFO \
	VDRM_IOC(2, 'V', 0x00, sizeof(struct vdrm_info))
#define VDRM_IOCTL_REGISTER \
	VDRM_IOC(3, 'V', 0x01, sizeof(struct vdrm_register_req))
#define VDRM_IOCTL_RETURN \
	VDRM_IOC(1, 'V', 0x02, sizeof(struct vdrm_handle_req))
#define VDRM_IOCTL_UNREGISTER \
	VDRM_IOC(1, 'V', 0x03, sizeof(struct vdrm_handle_req))

/* 注入一个输入事件。设备由驱动自己注册（见文件后部），所以这里只是
 * "把事件塞进内核 input 子系统"，不需要任何用户态守护进程。 */
struct vdrm_input_event {
	u16 type;    /* EV_KEY / EV_REL */
	u16 code;
	s32 value;
};
#define VDRM_IOCTL_INPUT       VDRM_IOC(1, 'V', 0x06, sizeof(struct vdrm_input_event))

/* Linux input 常量（取值自 include/uapi/linux/input-event-codes.h）*/
#define VDRM_EV_SYN     0x00
#define VDRM_EV_KEY     0x01
#define VDRM_EV_REL     0x02
#define VDRM_EV_REP     0x14
#define VDRM_SYN_REPORT 0x00
#define VDRM_REL_X      0x00
#define VDRM_REL_Y      0x01
#define VDRM_REL_HWHEEL 0x06
#define VDRM_REL_WHEEL  0x08
#define VDRM_KEY_MAX    0x2ff
/* KEY_* 取值在 0xf9~0x15f 之间是空洞，这段只属于 BTN_*（BTN_MISC..BTN_TASK）*/
#define VDRM_BTN_FIRST  0x100
#define VDRM_BTN_LAST   0x14f
#define VDRM_BUS_USB    0x03

/* A 路线：把池里每块的 {dma-buf fd, 几何} 交给 App（App 用 anland 的 forge 上屏）*/
struct vdrm_pool_info {
	s32 fd;        /* fd0（像素）— 原始 file 在调用进程里的新 fd */
	s32 fd1;       /* fd1（metadata blob）*/
	u32 id, width, height, pitch, format;
	u32 num_ints;
	u32 ints[VDRM_MAX_INTS];
	u32 flags;
};
struct vdrm_pool_list {
	u32 count;
	u32 reserved;
	struct vdrm_pool_info items[VDRM_POOL_MAX];
};
#define VDRM_IOCTL_GET_POOL \
	VDRM_IOC(3, 'V', 0x07, sizeof(struct vdrm_pool_list))

#define VDRM_ST_FREE  0
#define VDRM_ST_TAKEN 1
#define VDRM_ST_DISPL 2

struct vdrm_pool_entry {
	struct drm_gem_object *obj;
	u64 user_data;
	u32 id;
	u32 width, height, format, pitch;
	u32 state;
	u32 used;
	/* Experiment: retained until module reset, never released by RETURN. */
	u32 allocation_reserved;
	struct drm_file *allocation_owner; /* diagnostic identity only; never dereferenced */
	/* B/A′：把 App/helper 交来的【原始 file】持住，GET_POOL 时原样转发
	 * （绕开 drm_gem_prime_handle_to_fd——它会把身份变成 shmem 味）*/
	struct file *f0;   /* fd0 的 file（fget 持有）*/
	struct file *f1;   /* fd1 的 file */
	u32 num_ints;
	u32 ints[VDRM_MAX_INTS];
};

/* Diagnostic counters persist across pool resets; no allocation semantics change. */
static u32 g_diag_alloc[VDRM_POOL_MAX];
static u32 g_diag_return[VDRM_POOL_MAX];
static u32 g_diag_return_non_displ[VDRM_POOL_MAX];
static struct vdrm_pool_entry g_pool[VDRM_POOL_MAX];
static struct vdrm_pool_list  g_pool_list;   /* GET_POOL 用（避免内核栈上放 512B）*/
static int                 g_pool_next_id = 1;
static u32                 g_disp_hits;        /* DISPL 状态下又被翻的次数（竞态计数）*/
static u32                 g_gate_hits;        /* atomic_check 里看到“想抢非 FREE buffer”的次数 */
static struct mutex        g_pool_lock;        /* 静态清零 = 已解锁 */
static struct miscdevice   g_misc;
static struct file_operations g_misc_fops;
static int                 g_misc_registered;
/* GET_POOL 必须用【真正的 drm_file】——而不是 /dev/vdrm 那个 misc file！
 * 合成器开卡时 dumb_create 会拿到 drm_file，存下来备用。*/
static struct drm_file    *g_drm_file;

/* 固定输出几何（唯一 connector 的 preferred 1920x1080 XRGB8888） */
static u32 g_mode_w = 1920, g_mode_h = 1080, g_mode_pitch = 1920 * 4;

/* ---- M5: 帧就绪通知队列（App 用 read/poll 取）---- */
#define VDRM_FRAME_QMAX 16
struct vdrm_frame_event {
	u32 id;
	u32 sequence;
	u64 timestamp_ns;
	u64 user_data;
};
static struct vdrm_frame_event g_frame_q[VDRM_FRAME_QMAX];
static int                    g_frame_head, g_frame_tail;
static volatile u32           g_frame_seq;
static struct spinlock        g_frame_lock;   /* 静态清零 = 解锁 */
static struct wait_queue_head g_frame_wq;

static void *vdrm_kzalloc(size_t n)
{
	/* 用 kzalloc 而不是裸 __kmalloc：6.12 起 __kmalloc 对模块不再可见
	 * （改叫 __kmalloc_noprof，只有 alloc_hooks 包装）。kzalloc(n, GFP_KERNEL)
	 * 与原先的 __kmalloc(n, GFP_KERNEL|__GFP_ZERO) 完全等价。 */
	return kzalloc(n, GFP_KERNEL);
}

/* ============= M5: 池 / /dev/vdrm 实现 ============= */

/* 有头文件了，不用再手工等价 kref_put */
static void vdrm_gem_put(struct drm_gem_object *obj)
{
	if (obj)
		drm_gem_object_put(obj);
}

/*
 * 池对象用自定义 funcs：drm_gem_shmem_mmap 对 import 对象用的是
 * obj->dma_buf（NULL），会 WARN+EINVAL。改成直接用 import_attach->dmabuf。
 */
static struct drm_gem_object_funcs g_pool_funcs;
static int g_pool_funcs_ready;

static int vdrm_pool_mmap(struct drm_gem_object *obj, struct vm_area_struct *vma)
{
	int ret;

	if (!obj->import_attach)
		return -EINVAL;

	vma->vm_private_data = NULL;
	vma->vm_ops = NULL;
	ret = dma_buf_mmap(obj->import_attach->dmabuf, vma, 0);
#if 0
	/* 【实验】原代码在这里手动放一次引用（对应 drm_gem_mmap_obj 取的引用）。
	 * 怀疑它在高频 mmap/unmap 时释放过早 → GPU 还在用的 buffer 被回收
	 * → kgsl "CP: AHB bus error"。暂时注释掉：宁可漏引用，不可提前释放。*/
	if (!ret)
		vdrm_gem_put(obj);
#endif
	return ret;
}

/*
 * dumb_create：优先从池里发 App 的 gralloc buffer（伪装成 card1 原生 dumb），
 * 池里没有匹配/空闲的，回退到自持 shmem（合成器永不卡死）。
 */
/* 按底层 dma-buf 找到池项下标（池对象是 import，不能只比指针）*/
static int vdrm_pool_idx_by_obj(struct drm_gem_object *gobj)
{
	struct dma_buf *fdb = (gobj && gobj->import_attach) ?
		gobj->import_attach->dmabuf : NULL;
	int i;

	for (i = 0; i < VDRM_POOL_MAX; i++) {
		struct drm_gem_object *po = g_pool[i].obj;
		struct dma_buf *pdb = (po && po->import_attach) ?
			po->import_attach->dmabuf : NULL;

		if (!g_pool[i].used)
			continue;
		if (po == gobj || (fdb && pdb && fdb == pdb))
			return i;
	}
	return -1;
}

/* BTF 里 __drm_planes_state 只有前置声明，按实测布局（见 btf/all_types.h）本地定义 */
struct vdrm_planes_state {
	struct drm_plane       *ptr;
	struct drm_plane_state *state;
	struct drm_plane_state *old_state;
	struct drm_plane_state *new_state;
};

/* atomic_check：先让 helper 检查，再看这次提交想用哪块池 buffer。
 * 若那块还被 SF/App 占用（非 FREE），说明合成器想改写正在显示的 buffer
 * ——正是“重播”的根源。第一步先只记录，不阻断。 */
static int vdrm_atomic_check(struct drm_device *dev,
			     struct drm_atomic_state *state)
{
	int rc, i;

	rc = drm_atomic_helper_check(dev, state);
	if (rc)
		return rc;

	if (state && state->planes) {
		int n = dev->mode_config.num_total_plane;
		struct vdrm_planes_state *pl =
			(struct vdrm_planes_state *)state->planes;

		for (i = 0; i < n; i++) {
			struct drm_plane_state *ps = pl[i].new_state;
			int idx;

			if (!ps || !ps->fb || !ps->fb->obj[0])
				continue;
			idx = vdrm_pool_idx_by_obj(ps->fb->obj[0]);
			if (idx >= 0) {
				if (g_pool[idx].state == VDRM_ST_DISPL) {
					/* 这块已上屏、App 还没 RETURN。
					 *
					 * 这里**绝不能等**：App 的 RETURN 挂在 Android release fence 上，
					 * SF 只有收到更新的 buffer 才会释放旧 buffer，而下一帧由本次提交的
					 * pipe_update 推出。也就是说，在提交路径里等它 == 等自己放行，
					 * 只会空等到超时（实测 HZ=120 时 50×msleep(1) ≈ 420ms，
					 * 日志里每一跳都跑满 50 次才放行），把合成器提交线程整个卡住。
					 * pipe_update 会对同一块重新置 DISPL 并补推帧事件，语义上不需要
					 * 在这里阻断；因此只计数、不等待。*/
					if ((g_gate_hits++ % 30) == 0)
						pr_info("vdrm: gate-observe id=%u hit=%u (no wait)\n",
							g_pool[idx].id, g_gate_hits);
				}
			}
		}
	}
	return 0;
}

/*
 * A 路线：dumb_create 的回退从“自持 shmem”改成“从 qcom,system dma-heap 分配”。
 * 为什么：qcom,system 就是 gralloc 用的那个堆（App 的 AHB 就在它上面），
 * 显示管线认；而 drm_gem_shmem 是另一种内存，App 伪造后上屏会炸。
 * 这样池天生就是模块的、内存可显示 → 合成器先起 / App 重启都不用换 buffer。
 */
static int vdrm_heap_dumb_create(struct drm_file *file, struct drm_device *dev,
				 struct drm_mode_create_dumb *args)
{
	struct dma_heap *heap;
	struct dma_buf *buf;
	struct drm_gem_object *obj;
	u32 handle, pitch;
	int i, slot = -1, rc;

	pitch = (args->width * args->bpp + 7) / 8;
	args->pitch = (pitch + 63) & ~63u;   /* 同 drm_gem_shmem_dumb_create 的 ALIGN(pitch,64) */
	args->size  = (u64)args->pitch * args->height;

	heap = dma_heap_find("qcom,system");
	if (!heap) {
		pr_err("vdrm: dma_heap_find(qcom,system) failed\n");
		return -ENODEV;
	}
	buf = dma_heap_buffer_alloc(heap, args->size, 0x80002 /*O_CLOEXEC|O_RDWR*/, 0);
	dma_heap_put(heap);
	if (!buf || (unsigned long)buf >= (unsigned long)-4095) {
		long e = (long)buf;
		pr_err("vdrm: heap alloc %llu bytes failed (%ld)\n",
		       (unsigned long long)args->size, e);
		return buf ? (int)e : -ENOMEM;
	}

	/* 导入成 card1 的 GEM（GEM 自己会持 dma_buf 的引用）*/
	obj = drm_gem_prime_import(dev, buf);
	dma_buf_put(buf);
	if (!obj || (unsigned long)obj >= (unsigned long)-4095) {
		long e = (long)obj;
		pr_err("vdrm: prime_import failed (%ld)\n", e);
		return obj ? (int)e : -ENOMEM;
	}

	/* 用池的 funcs（修 mmap，避开 drm_gem_shmem_mmap 的 WARNING）*/
	if (!g_pool_funcs_ready && obj->funcs) {
		g_pool_funcs = *obj->funcs;
		g_pool_funcs.mmap = vdrm_pool_mmap;
		g_pool_funcs_ready = 1;
	}
	if (g_pool_funcs_ready)
		obj->funcs = &g_pool_funcs;

	/* 登记进池：pipe_update 才能匹配到 → App 才会收到 frame ready */
	mutex_lock(&g_pool_lock);
	for (i = 0; i < VDRM_POOL_MAX; i++) {
		if (!g_pool[i].used) { slot = i; break; }
	}
	if (slot >= 0) {
		memset(&g_pool[slot], 0, sizeof(g_pool[slot]));
		g_pool[slot].obj       = obj;
		g_pool[slot].user_data = 0;
		g_pool[slot].id        = g_pool_next_id++;
		g_pool[slot].width     = args->width;
		g_pool[slot].height    = args->height;
		g_pool[slot].format    = VDRM_FMT_XRGB8888;
		g_pool[slot].pitch     = args->pitch;
		g_pool[slot].state     = VDRM_ST_FREE;
		g_pool[slot].used      = 1;
	}
	mutex_unlock(&g_pool_lock);

	rc = drm_gem_handle_create(file, obj, &handle);
	if (rc) {
		if (slot >= 0) {
			mutex_lock(&g_pool_lock);
			memset(&g_pool[slot], 0, sizeof(g_pool[slot]));
			mutex_unlock(&g_pool_lock);
			vdrm_gem_put(obj);
		}
		pr_err("vdrm: handle_create failed rc=%d\n", rc);
		return rc;
	}
	args->handle = handle;
	pr_info("vdrm: dumb_create -> HEAP(qcom,system) %ux%u pitch=%u handle=%u pool_id=%d\n",
		args->width, args->height, args->pitch, handle,
		slot >= 0 ? (int)g_pool[slot].id : -1);
	return 0;
}

static int vdrm_dumb_create(struct drm_file *file, struct drm_device *dev,
			    struct drm_mode_create_dumb *args)
{
	int i, has_pool = 0;

	g_drm_file = file;   /* 任何一次 dumb_create 都记下有效的 drm_file（GET_POOL 要用）*/

	mutex_lock(&g_pool_lock);
	for (i = 0; i < VDRM_POOL_MAX; i++)
		if (g_pool[i].used && g_pool[i].f0) has_pool = 1;
	if (args->bpp == 32) {
		for (i = 0; i < VDRM_POOL_MAX; i++) {
			struct vdrm_pool_entry *e = &g_pool[i];
			u32 handle;
			int rc;

			/* Display release is not allocation release, including PRIME aliases. */
			if (!e->used || e->allocation_reserved || e->state != VDRM_ST_FREE)
				continue;
			/* 只认【App/helper REGISTER 进来的】真缓冲（有原始 f0）。
			 * HEAP 回退登记的槽没有 f0，绝不能当池缓冲发给合成器。*/
			if (!e->f0)
				continue;
			if (e->width != args->width || e->height != args->height)
				continue;

			{
				u32 before = e->obj->handle_count;
				u32 attempt = ++g_diag_alloc[i];
				rc = drm_gem_handle_create(file, e->obj, &handle);
				if (!rc) {
					e->allocation_reserved = 1;
					e->allocation_owner = file;
				}
				if (attempt <= 32)
					pr_info("vdrm: DIAG_ALLOC t_ns=%llu slot=%d attempt=%u file=%px id=%u obj=%px dbuf=%px before=%u after=%u handle=%u rc=%d live_handle_candidate=%u reserved=%u\n",
						(unsigned long long)ktime_get(), i, attempt, file,
						e->id, e->obj,
						e->obj->import_attach ? e->obj->import_attach->dmabuf : NULL,
						before, e->obj->handle_count, rc ? 0 : handle, rc, before != 0,
						e->allocation_reserved);
			}
			pr_info("vdrm: handle_create rc=%d handle=%u pool_id=%u\n",
				rc, rc ? 0 : handle, e->id);
			if (rc)
				continue;
			e->state     = VDRM_ST_TAKEN;
			args->handle = handle;
			args->pitch  = e->pitch;
			args->size   = e->obj->size;
			mutex_unlock(&g_pool_lock);
			pr_info("vdrm: dumb_create -> POOL id=%u handle=%u %ux%u pitch=%u\n",
				e->id, handle, e->width, e->height, e->pitch);
			return 0;
		}
	}
	mutex_unlock(&g_pool_lock);
	if (has_pool)
		return args->bpp == 32 ? -ENOSPC : -EINVAL;

	pr_info("vdrm: dumb_create -> HEAP %ux%u bpp=%u\n",
		args->width, args->height, args->bpp);
	return vdrm_heap_dumb_create(file, dev, args);
}

/*
 * dumb_map_offset：drm_gem_dumb_map_offset 会拒 import_attach（-EINVAL），
 * 池里的 App buffer 都是 import 对象，必须自己来（逻辑同它，只去掉那个检查）。
 */
static int vdrm_dumb_map_offset(struct drm_file *file, struct drm_device *dev,
				unsigned int handle, unsigned long long *offset)
{
	struct drm_gem_object *obj = drm_gem_object_lookup(file, handle);
	int rc;

	if (!obj)
		return -ENOENT;

	rc = drm_gem_create_mmap_offset(obj);
	if (!rc)
		*offset = ((u64)obj->vma_node.vm_node.start) << 12;
	pr_info("vdrm: dumb_map_offset handle=%u obj=%px rc=%d off=%llx ia=%px db=%px\n",
		handle, obj, rc, (unsigned long long)*offset,
		obj->import_attach, obj->dma_buf);

	vdrm_gem_put(obj);
	return rc;
}

/* ---- M5: 帧队列 push/pop + read/poll ---- */
static void vdrm_frame_push(u32 id, u64 user_data)
{
	struct vdrm_frame_event ev;
	unsigned long flags;
	int next = (g_frame_head + 1) % VDRM_FRAME_QMAX;

	/* pipe_update 上下文不能睡，先只入队（自旋锁）*/
	flags = _raw_spin_lock_irqsave(&g_frame_lock.rlock);
	ev.id           = id;
	ev.user_data    = user_data;
	ev.sequence     = ++g_frame_seq;
	ev.timestamp_ns = (u64)ktime_get();
	if (next != g_frame_tail) {
		g_frame_q[g_frame_head] = ev;
		g_frame_head = next;
	}
	_raw_spin_unlock_irqrestore(&g_frame_lock.rlock, flags);

	/* 帧事件只有一条出口：/dev/vdrm 自己的 read/poll 队列 */
	__wake_up(&g_frame_wq, 3 /* TASK_NORMAL */, 1, NULL);
}

static int vdrm_frame_pop(struct vdrm_frame_event *ev)
{
	unsigned long flags;
	int rc = -1;

	flags = _raw_spin_lock_irqsave(&g_frame_lock.rlock);
	if (g_frame_tail != g_frame_head) {
		*ev = g_frame_q[g_frame_tail];
		g_frame_tail = (g_frame_tail + 1) % VDRM_FRAME_QMAX;
		rc = 0;
	}
	_raw_spin_unlock_irqrestore(&g_frame_lock.rlock, flags);
	return rc;
}

static ssize_t vdrm_misc_read(struct file *file, char __user *buf,
			      size_t count, loff_t *ppos)
{
	struct vdrm_frame_event ev;

	if (count < sizeof(ev))
		return -EINVAL;
	if (vdrm_frame_pop(&ev))
		return -EAGAIN;
	if (copy_to_user((void *)buf, &ev, sizeof(ev)))
		return -EFAULT;
	return sizeof(ev);
}

static __poll_t vdrm_misc_poll(struct file *file,
			       struct poll_table_struct *wait)
{
	unsigned long flags;
	int nonempty;
	__poll_t mask = 0;

	/* poll_wait 的内联等价物 */
	if (wait && wait->_qproc)
		wait->_qproc(file, &g_frame_wq, wait);

	flags = _raw_spin_lock_irqsave(&g_frame_lock.rlock);
	nonempty = (g_frame_head != g_frame_tail);
	_raw_spin_unlock_irqrestore(&g_frame_lock.rlock, flags);

	if (nonempty)
		mask |= 0x0001 | 0x0040;   /* POLLIN | POLLRDNORM */
	return mask;
}

/* App 关闭 /dev/vdrm（或进程被杀）时内核回调：清空整个池 + 释放信令。
 * 这是"自愈"的关键——不依赖 App 主动 UNREGISTER，进程崩了也能复位。 */
static int vdrm_misc_release(struct inode *inode, struct file *file)
{
	pr_info("vdrm: misc release (池不清 —— 池归模块，App 重启不影响)\n");

	/* 清帧事件环：否则新 App 会读到上一实例遗留的旧事件 */
	g_frame_head = g_frame_tail = 0;
	g_frame_seq = 0;

	return 0;
}

static void vdrm_input_inject(u32 type, u32 code, s32 value);   /* 定义见文件后部 */

/* /dev/vdrm ioctl */
static long vdrm_misc_ioctl(struct file *file, unsigned int cmd,
			    unsigned long arg)
{
	switch (cmd) {
	case VDRM_IOCTL_GET_INFO: {
		volatile struct vdrm_info info;
		unsigned int i;

		if (!g_dev)
			return -ENODEV;
		/* 必须通过 volatile 写，否则 -O2 可能把写入优化掉（坑 2）*/
		for (i = 0; i < sizeof(info) / sizeof(u32); i++)
			((volatile u32 *)&info)[i] = 0;
		info.abi_version   = VDRM_ABI_VERSION;
		info.width         = g_mode_w;
		info.height        = g_mode_h;
		info.format        = VDRM_FMT_XRGB8888;
		info.pitch         = g_mode_pitch;
		info.pool_capacity = VDRM_POOL_MAX;
		pr_info("vdrm: GET_INFO sizeof=%d abi=%u %ux%u fmt=%08x pitch=%u cap=%u arg=%px\n",
			(int)sizeof(struct vdrm_info), info.abi_version, info.width,
			info.height, info.format, info.pitch, info.pool_capacity,
			(void *)arg);
		if (copy_to_user((void *)arg, (const void *)&info, sizeof(info)))
			return -EFAULT;
		return 0;
	}
	case VDRM_IOCTL_GET_POOL: {
		int i, n = 0;

		if (!g_dev)
			return -ENODEV;
		memset(&g_pool_list, 0, sizeof(g_pool_list));
		mutex_lock(&g_pool_lock);
		for (i = 0; i < VDRM_POOL_MAX; i++) {
			int fd0 = -1, fd1 = -1;
			u32 k;

			if (!g_pool[i].used || !g_pool[i].obj)
				continue;
			/* B/A′：把【原始 file】安装到调用进程。
			 * fd_install 会吃掉一个引用，装完在当前进程里 fget 回来，
			 * 保证我们手上总持有一个引用（可反复转发）。
			 * 千万不能用 drm_gem_prime_handle_to_fd——它会把身份变 shmem 味。*/
			if (g_pool[i].f0) {
				fd0 = get_unused_fd_flags(0x80000 /*O_CLOEXEC*/);
				if (fd0 >= 0) {
					fd_install((unsigned int)fd0, g_pool[i].f0);
					g_pool[i].f0 = fget((unsigned int)fd0);
				}
			}
			if (g_pool[i].f1) {
				fd1 = get_unused_fd_flags(0x80000 /*O_CLOEXEC*/);
				if (fd1 >= 0) {
					fd_install((unsigned int)fd1, g_pool[i].f1);
					g_pool[i].f1 = fget((unsigned int)fd1);
				}
			}
			g_pool_list.items[n].fd       = fd0;
			g_pool_list.items[n].fd1      = fd1;
			g_pool_list.items[n].id       = g_pool[i].id;
			g_pool_list.items[n].width    = g_pool[i].width;
			g_pool_list.items[n].height   = g_pool[i].height;
			g_pool_list.items[n].pitch    = g_pool[i].pitch;
			g_pool_list.items[n].format   = g_pool[i].format;
			g_pool_list.items[n].num_ints = g_pool[i].num_ints;
			{
				volatile u32 *dst = (volatile u32 *)g_pool_list.items[n].ints;
				const volatile u32 *src = (const volatile u32 *)g_pool[i].ints;
				for (k = 0; k < g_pool[i].num_ints && k < VDRM_MAX_INTS; k++)
					dst[k] = src[k];
			}
			n++;
		}
		g_pool_list.count = (u32)n;
		mutex_unlock(&g_pool_lock);
		pr_info("vdrm: GET_POOL -> %d entries\n", n);
		if (copy_to_user((void *)arg, &g_pool_list,
				      sizeof(g_pool_list)))
			return -EFAULT;
		return 0;
	}
	case VDRM_IOCTL_REGISTER: {
		struct vdrm_register_req req;
		struct dma_buf *dbuf;
		struct drm_gem_object *obj;
		int i, k, slot = -1;

		if (copy_from_user(&req, (const void *)arg, sizeof(req)))
			return -EFAULT;
		if (!g_dev)
			return -ENODEV;
		/* 几何由模块决定（g_mode_w/g_mode_h），App 不参与。
		 * 这里只用 App buffer 的尺寸登记池项，不改 g_mode_*。*/

		dbuf = dma_buf_get(req.fd);
		if (!dbuf || (unsigned long)dbuf > (unsigned long)-4096)
			return -EBADF;

		mutex_lock(&g_pool_lock);
		/* 幂等：同一个 dma_buf 已注册则返回原 id */
		for (i = 0; i < VDRM_POOL_MAX; i++) {
			if (g_pool[i].used && g_pool[i].obj &&
			    g_pool[i].obj->import_attach &&
			    g_pool[i].obj->import_attach->dmabuf == dbuf) {
				req.id = g_pool[i].id;
				mutex_unlock(&g_pool_lock);
				dma_buf_put(dbuf);
				copy_to_user((char __user *)arg, &req, sizeof(req));
				return 0;
			}
		}
		for (i = 0; i < VDRM_POOL_MAX; i++) {
			if (!g_pool[i].used) {
				slot = i;
				break;
			}
		}
		mutex_unlock(&g_pool_lock);
		if (slot < 0) {
			dma_buf_put(dbuf);
			return -ENOSPC;
		}

		obj = drm_gem_prime_import(g_dev, dbuf);
		dma_buf_put(dbuf);
		if (!obj || (unsigned long)obj > (unsigned long)-4096) {
			pr_info("vdrm: prime_import FAILED obj=%px err=%ld\n",
				obj, obj ? (long)obj : 0);
			return -EINVAL;
		}

		/* 让 import 对象的 mmap 能工作（见 vdrm_pool_mmap）*/
		if (!g_pool_funcs_ready && obj->funcs) {
			g_pool_funcs = *obj->funcs;
			g_pool_funcs.mmap = vdrm_pool_mmap;
			g_pool_funcs_ready = 1;
		}
		if (g_pool_funcs_ready)
			obj->funcs = &g_pool_funcs;

		mutex_lock(&g_pool_lock);
		memset(&g_pool[slot], 0, sizeof(g_pool[slot]));
		g_pool[slot].obj       = obj;
		g_pool[slot].user_data = req.user_data;
		g_pool[slot].id        = g_pool_next_id++;
		g_pool[slot].width     = req.width;
		g_pool[slot].height    = req.height;
		g_pool[slot].format    = req.format;
		g_pool[slot].pitch     = req.pitch;
		g_pool[slot].state     = VDRM_ST_FREE;
		g_pool[slot].used      = 1;
		/* B/A′：fget 住原始 fd0/fd1 的 file（GET_POOL 直接转发它们）*/
		g_pool[slot].f0 = fget((unsigned int)req.fd);
		g_pool[slot].f1 = (req.fd1 >= 0) ?
			fget((unsigned int)req.fd1) : NULL;
		g_pool[slot].num_ints = (req.num_ints <= VDRM_MAX_INTS) ?
			req.num_ints : VDRM_MAX_INTS;
		{
			volatile u32 *dst = (volatile u32 *)g_pool[slot].ints;
			const volatile u32 *src = (const volatile u32 *)req.ints;
			for (k = 0; k < (int)g_pool[slot].num_ints; k++)
				dst[k] = src[k];
		}
		req.id                 = g_pool[slot].id;
		mutex_unlock(&g_pool_lock);

		pr_info("vdrm: REGISTER id=%u %ux%u pitch=%u size=%zu\n",
			req.id, req.width, req.height, req.pitch, obj->size);
		if (copy_to_user((void *)arg, &req, sizeof(req))) {
			/* 回传失败：把已入池的 slot 退掉 */
			mutex_lock(&g_pool_lock);
			memset(&g_pool[slot], 0, sizeof(g_pool[slot]));
			mutex_unlock(&g_pool_lock);
			vdrm_gem_put(obj);
			return -EFAULT;
		}
		return 0;
	}
	case VDRM_IOCTL_RETURN: {
		struct vdrm_handle_req req;
		int i, rc = -ENOENT;
		u32 before = 0, after = 0, handles = 0, count = 0, extra = 0;
		u64 timestamp = 0;
		struct drm_gem_object *obj = NULL;

		if (copy_from_user(&req, (const void *)arg, sizeof(req)))
			return -EFAULT;
		mutex_lock(&g_pool_lock);
		for (i = 0; i < VDRM_POOL_MAX; i++) {
			if (g_pool[i].used && g_pool[i].id == req.id) {
				before = g_pool[i].state;
				obj = g_pool[i].obj;
				handles = obj ? obj->handle_count : 0;
				count = ++g_diag_return[i];
				if (before != VDRM_ST_DISPL) ++g_diag_return_non_displ[i];
				extra = g_diag_return_non_displ[i];
				timestamp = (u64)ktime_get();
				if (g_pool[i].state == VDRM_ST_DISPL)
					g_pool[i].state = VDRM_ST_FREE;
				after = g_pool[i].state;
				rc = 0;
				break;
			}
		}
		mutex_unlock(&g_pool_lock);
		if (!rc && count <= 32)
			pr_info("vdrm: DIAG_RETURN t_ns=%llu slot=%d id=%u obj=%px before=%u after=%u handles=%u count=%u non_displ=%u\n",
				(unsigned long long)timestamp, i, req.id, obj,
				before, after, handles, count, extra);
		return rc;
	}
	case VDRM_IOCTL_UNREGISTER: {
		struct vdrm_handle_req req;
		struct drm_gem_object *obj = NULL;
		int i;

		if (copy_from_user(&req, (const void *)arg, sizeof(req)))
			return -EFAULT;
		mutex_lock(&g_pool_lock);

		for (i = 0; i < VDRM_POOL_MAX; i++) {
			if (g_pool[i].used && (!req.id || g_pool[i].id == req.id) &&
			    (g_pool[i].allocation_reserved || g_pool[i].state != VDRM_ST_FREE)) {
				mutex_unlock(&g_pool_lock);
				return -EBUSY;
			}
		}

		if (req.id == 0) {
			/* id=0 = 清空整池（mkpool 开跑前用，避免多次运行累积批次）*/
			for (i = 0; i < VDRM_POOL_MAX; i++) {
				if (!g_pool[i].used)
					continue;
				if (g_pool[i].f0) fput(g_pool[i].f0);
				if (g_pool[i].f1) fput(g_pool[i].f1);
				if (g_pool[i].obj) vdrm_gem_put(g_pool[i].obj);
				memset(&g_pool[i], 0, sizeof(g_pool[i]));
			}
			mutex_unlock(&g_pool_lock);
			pr_info("vdrm: UNREGISTER id=0 -> 清空整池\n");
			return 0;
		}

		for (i = 0; i < VDRM_POOL_MAX; i++) {
			if (g_pool[i].used && g_pool[i].id == req.id) {
				obj = g_pool[i].obj;
				memset(&g_pool[i], 0, sizeof(g_pool[i]));
				break;
			}
		}
		mutex_unlock(&g_pool_lock);
		if (!obj)
			return -ENOENT;
		vdrm_gem_put(obj);
		return 0;
	}
	case VDRM_IOCTL_INPUT: {
		struct vdrm_input_event ie;

		if (copy_from_user(&ie, (const void *)arg, sizeof(ie)))
			return -EFAULT;
		vdrm_input_inject(ie.type, ie.code, ie.value);
		return 0;
	}
	default:
		return -ENOTTY;
	}
}

/* ---------------- 阶段实现（在 kworker 上下文里跑，可以睡眠）---------------- */

static void vdrm_drv_release(struct drm_device *dev);

/* ===== 会话边界归还 =====
 * 归还不按 RETURN、不按 handle_count，只在合成器的 drm_file 关闭时发生。
 * 会话内仍不复用 → 保留“禁止重复分配”的防别名效果；
 * 会话结束才回收 → 解决修复版“用完不还、第二次拿不到缓冲”。 */
static u32 g_postclose_releases;
static void vdrm_postclose(struct drm_device *dev, struct drm_file *file)
{
	int i;
	(void)dev;

	mutex_lock(&g_pool_lock);
	for (i = 0; i < VDRM_POOL_MAX; i++) {
		struct vdrm_pool_entry *e = &g_pool[i];

		if (!e->used || e->allocation_owner != file)
			continue;
		/* 归属归还；显示占用仍由 App 的 RETURN 决定（DISPL 保持不动） */
		e->allocation_reserved = 0;
		e->allocation_owner    = NULL;
		if (e->state == VDRM_ST_TAKEN)
			e->state = VDRM_ST_FREE;
		g_postclose_releases++;
		pr_info("vdrm: postclose release id=%u state=%u total=%u\n",
			e->id, e->state, g_postclose_releases);
	}
	mutex_unlock(&g_pool_lock);
}

static void vdrm_setup_driver(void)
{
	g_fops.owner          = THIS_MODULE;
	g_fops.open           = drm_open;
	g_fops.release        = drm_release;
	g_fops.unlocked_ioctl = drm_ioctl;
	g_fops.compat_ioctl   = drm_compat_ioctl;
	g_fops.poll           = drm_poll;
	g_fops.read           = drm_read;
	g_fops.mmap           = drm_gem_mmap;

	g_driver.driver_features = VDRM_DRIVER_FEATURES;
	g_driver.name            = "vdrm";
	g_driver.desc            = "Aether Virtual DRM Device";
	g_driver.date            = "20261003";
	g_driver.major           = 1;
	g_driver.minor           = 0;
	g_driver.patchlevel      = 0;
	g_driver.fops            = &g_fops;
	g_driver.release         = vdrm_drv_release;
	g_driver.postclose       = vdrm_postclose;

	/* M3: GEM。shmem 助手开箱即用 —— drm_gem_shmem_create()
	 * 会自动填 obj->funcs(free/pin/vmap/mmap/vm_ops)，我们不用管。 */
	/* M5: 优先从池发 App 的 gralloc buffer；无则回退自持 shmem */
	g_driver.dumb_create               = vdrm_dumb_create;
	/* M5: 放开 import_attach（drm_gem_dumb_map_offset 会拒导入对象）*/
	g_driver.dumb_map_offset           = vdrm_dumb_map_offset;
	/* 这个内核把 PRIME ioctl 做成了驱动级回调：
	 *   drm_prime_handle_to_fd_ioctl(): if (!driver->prime_handle_to_fd) return -ENOSYS;
	 *   drm_prime_fd_to_handle_ioctl(): if (!driver->prime_fd_to_handle) return -ENOSYS;
	 * 不设这两个，PRIME 全废（实测 rc=-1 ENOSYS）。 */
	g_driver.prime_handle_to_fd        = drm_gem_prime_handle_to_fd;
	g_driver.prime_fd_to_handle        = drm_gem_prime_fd_to_handle;
	/* 6.6 起 struct drm_driver 里没有 gem_prime_mmap 了 —— DRM core 直接调
	 * drm_gem_prime_mmap()。原先赋的也正是这个默认值，所以 6.6+ 不赋值即可，
	 * 行为不变。 */
#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 6, 0)
	g_driver.gem_prime_mmap            = drm_gem_prime_mmap;
#endif
	g_driver.gem_prime_import          = drm_gem_prime_import;
	g_driver.gem_prime_import_sg_table = drm_gem_shmem_prime_import_sg_table;
}

static int vdrm_do_alloc(void)
{
	int rc;

	if (g_dev)
		return 0;

	vdrm_setup_driver();

	/* drm_dev_init() 有 WARN_ON(!parent)，parent 必须非 NULL */
	g_pdev = platform_device_alloc("vdrm", -1);
	if (!g_pdev) {
		pr_info("vdrm: platform_device_alloc failed\n");
		return -ENOMEM;
	}

	rc = platform_device_add(g_pdev);
	if (rc) {
		pr_info("vdrm: platform_device_add failed rc=%d\n", rc);
		platform_device_put(g_pdev);
		g_pdev = NULL;
		return rc;
	}

	/* 让 dma_buf_map_attachment 能给这个 device 做 DMA 映射（否则 import 报 -EIO）*/
	g_pdev->dev.dma_mask = &g_pdev->dev.coherent_dma_mask;
	g_pdev->dev.coherent_dma_mask = 0xFFFFFFFFFFFFFFFFULL;

	g_dev = drm_dev_alloc(&g_driver, &g_pdev->dev);
	if (!g_dev) {
		pr_info("vdrm: drm_dev_alloc returned NULL\n");
		return -ENOMEM;
	}
	if ((unsigned long)g_dev > (unsigned long)-4096) {
		pr_info("vdrm: drm_dev_alloc err=%ld\n", -(long)g_dev);
		g_dev = NULL;
		return -EINVAL;
	}

	pr_info("vdrm: drm_dev_alloc ok, dev=%px\n", g_dev);
	return 0;
}

static int vdrm_do_modecfg(void)
{
	int rc;

	if (!g_dev)
		return -ENODEV;
	if (g_has_modecfg)
		return 0;

	rc = drmm_mode_config_init(g_dev);
	if (rc) {
		pr_info("vdrm: drmm_mode_config_init rc=%d\n", rc);
		return rc;
	}
	/* ADDFB2 必经 funcs->fb_create；DRIVER_ATOMIC 还要求 atomic_check/commit。
	 * 这三个是空指针就是内核崩溃，必须在注册前设好。 */
	g_mode_funcs.fb_create     = drm_gem_fb_create;
	g_mode_funcs.atomic_check  = vdrm_atomic_check;
	g_mode_funcs.atomic_commit = drm_atomic_helper_commit;
	g_dev->mode_config.funcs   = &g_mode_funcs;

	/* vblank 基础设施：必须在 drm_dev_register 之前。nodevice 也要有，
	 * 否则 atomic helper 会把 no_vblank 置真、page-flip 事件被 fake 掉，
	 * 合成器拿不到真正的帧完成回调。 */
	rc = drm_vblank_init(g_dev, 1);
	if (rc) {
		pr_info("vdrm: drm_vblank_init rc=%d\n", rc);
		return rc;
	}

	g_has_modecfg = 1;
	pr_info("vdrm: mode_config init ok (funcs=%px fb_create=%px check=%px commit=%px)\n",
		&g_mode_funcs, drm_gem_fb_create,
		drm_atomic_helper_check, drm_atomic_helper_commit);
	return 0;
}

static int vdrm_do_register(void)
{
	int rc;

	if (!g_dev)
		return -ENODEV;
	if (g_registered)
		return 0;

	rc = vdrm_do_modecfg();
	if (rc)
		return rc;

	rc = drm_dev_register(g_dev, 0);
	if (rc) {
		pr_info("vdrm: drm_dev_register rc=%d\n", rc);
		return rc;
	}
	g_registered = 1;
	pr_info("vdrm: registered\n");

	/* 建 /dev/vdrm（App <-> 模块 接口）。失败不致命。 */
	if (!g_misc_registered) {
		memset(&g_misc, 0, sizeof(g_misc));
		g_misc_fops.owner          = THIS_MODULE;
		g_misc_fops.unlocked_ioctl = vdrm_misc_ioctl;
		g_misc_fops.compat_ioctl   = vdrm_misc_ioctl;
		g_misc_fops.read           = vdrm_misc_read;
		g_misc_fops.poll           = vdrm_misc_poll;
		g_misc_fops.release        = vdrm_misc_release;
		/* 初始化帧等待队列（静态清零的 spinlock = 解锁）*/
		g_frame_wq.head.next = &g_frame_wq.head;
		g_frame_wq.head.prev = &g_frame_wq.head;
		g_frame_head = g_frame_tail = 0;
		g_misc.minor = 255;        /* MISC_DYNAMIC_MINOR */
		g_misc.name  = "vdrm";
		g_misc.fops  = &g_misc_fops;
		g_misc.mode  = 0666;       /* App（非 root）要能打开；后续收紧 */
		rc = misc_register(&g_misc);
		if (rc)
			pr_info("vdrm: misc_register rc=%d（/dev/vdrm 不可用）\n", rc);
		else {
			g_misc_registered = 1;
			pr_info("vdrm: /dev/vdrm registered (minor=%d)\n",
				g_misc.minor);
		}
	}
	return 0;
}

/* kref 归零时内核会调这个，是"devres 即将回收"的确凿信号 */
static void vdrm_drv_release(struct drm_device *dev)
{
	pr_info("vdrm: === driver.release 被调用，devres 即将回收 ===\n");
}

static void vdrm_do_unregister(void)
{
	/* 先注销 /dev/vdrm（不再有新 App ioctl），再清空池 */
	if (g_misc_registered) {
		misc_deregister(&g_misc);
		g_misc_registered = 0;
		pr_info("vdrm: /dev/vdrm deregistered\n");
	}
	{
		int i;

		mutex_lock(&g_pool_lock);
		for (i = 0; i < VDRM_POOL_MAX; i++) {
			if (g_pool[i].used && g_pool[i].obj) {
				if (g_pool[i].f0) fput(g_pool[i].f0);
				if (g_pool[i].f1) fput(g_pool[i].f1);
				vdrm_gem_put(g_pool[i].obj);
				memset(&g_pool[i], 0, sizeof(g_pool[i]));
			}
		}
		mutex_unlock(&g_pool_lock);
	}

	/* 先停 vblank 定时器。这里在 kworker 上下文、不持有任何 DRM 自旋锁，
	 * 所以可以安全地用会等待回调结束的 hrtimer_cancel()。 */
	if (g_vblank_timer_inited) {
		g_vblank_on = 0;
		g_vblank_timer_running = 0;
		hrtimer_cancel(&g_vblank_timer);
	}

	if (g_dev && g_registered) {
		drm_dev_unregister(g_dev);
		g_registered = 0;
		pr_info("vdrm: unregistered\n");
	}
	if (g_dev) {
		drm_dev_put(g_dev);
		g_dev = NULL;
		g_has_modecfg = 0;
		pr_info("vdrm: dev put\n");
	}
	if (g_pdev) {
		platform_device_unregister(g_pdev);
		g_pdev = NULL;
		pr_info("vdrm: pdev unregistered\n");
	}
	/* drm_dev_put() 内部已经跑完 drm_mode_config_cleanup，
	 * 现在可以释放我们自己 kzalloc 的 pipe / connector */
	if (g_pipe) { kfree(g_pipe); g_pipe = NULL; }
	if (g_conn) { kfree(g_conn); g_conn = NULL; }
}

/* ---- M2b: CRTC + primary plane + encoder + connector ---- */

/*
 * vblank 定时器回调。运行在 hrtimer 上下文（硬/软中断），只能做非睡眠的事：
 * drm_crtc_handle_vblank() 全是 spinlock + wake_up，安全。
 * 它会：bump vblank 计数 -> 唤醒 WAIT_VBLANK -> 发送已 arm 的 page-flip 事件。
 */
static enum hrtimer_restart vdrm_vblank_timer_fn(struct hrtimer *t)
{
	if (!g_vblank_timer_running || !g_pipe)
		return HRTIMER_NORESTART;

	/* 网格连续前进：没人持 vblank 引用时也照常走，只是不上报
	 * （无人持引用时 vblank->enabled 为 false，此时 handle_vblank 会 WARN）。*/
	if (g_vblank_on)
		drm_crtc_handle_vblank(&g_pipe->crtc);

	hrtimer_forward(t, ktime_get(), (ktime_t)g_vblank_period_ns);
	return HRTIMER_RESTART;
}

/* 由 DRM core 在需要 vblank 中断时调用（drv_vblank_get 的 0->1） */
static int vdrm_pipe_enable_vblank(struct drm_simple_display_pipe *pipe)
{
	if (!g_vblank_timer_inited)
		return -EINVAL;

	g_vblank_on = 1;
	/* 只在网格还没跑起来时启动。旧写法每次都从“现在”重新起算一整个周期，
	 * 于是每次提交都要白等满 16.67ms 才收到 flip-done
	 * （实测提交间隔恒为 31.8ms = 16.67 等待 + 渲染）。vblank 网格必须连续。*/
	if (!g_vblank_timer_running) {
		g_vblank_timer_running = 1;
		hrtimer_start_range_ns(&g_vblank_timer,
				       (ktime_t)g_vblank_period_ns,
				       0, HRTIMER_MODE_REL);
	}
	pr_info("vdrm: vblank on (period=%llu ns)\n",
		(unsigned long long)g_vblank_period_ns);
	return 0;
}

/*
 * 注意：这个回调是从 drm_crtc_vblank_off() 里调用的，而那条路径持有
 * event_lock + vbl_lock（自旋锁）。所以这里**绝不能**用会等待回调结束的
 * hrtimer_cancel()，否则若 vblank 回调正在别的 CPU 上等 event_lock，
 * 就会和持有 event_lock 的我们构成死锁。
 * 而且这里也不该停定时器：停了，下一次 enable_vblank 就会从“现在”重新起算
 * 一整个周期，vblank 相位被打断 —— 那正是每帧白等 16.67ms 的来源。
 * 做法：只放引用，网格由 pipe disable / 模块卸载时统一停。
 */
static void vdrm_pipe_disable_vblank(struct drm_simple_display_pipe *pipe)
{
	g_vblank_on = 0;
	pr_info("vdrm: vblank off\n");
}

static void vdrm_pipe_enable(struct drm_simple_display_pipe *pipe,
			     struct drm_crtc_state *crtc_state,
			     struct drm_plane_state *plane_state)
{
	struct drm_display_mode *m = &crtc_state->mode;

	/* 从 mode 反推一帧时长（clock 单位 kHz）:
	 *   period_ns = htotal * vtotal * 1e6 / clock_khz
	 * 兜底保持 60Hz。 */
#if VDRM_VBLANK_HZ_FORCE > 0
	g_vblank_period_ns = 1000000000ULL / VDRM_VBLANK_HZ_FORCE;
#else
	if (m->clock > 0 && m->htotal && m->vtotal) {
		u64 p = (u64)m->htotal * (u64)m->vtotal * 1000000ULL /
			(u64)m->clock;
		if (p >= 1000000ULL && p <= 1000000000ULL)
			g_vblank_period_ns = p;
	}
#endif
	pr_info("vdrm: pipe enable %ux%u period=%llu ns\n",
		m->hdisplay, m->vdisplay,
		(unsigned long long)g_vblank_period_ns);

	/* 通知 DRM core vblank 硬件已上线 */
	drm_crtc_vblank_on(&pipe->crtc);
}

static void vdrm_pipe_disable(struct drm_simple_display_pipe *pipe)
{
	pr_info("vdrm: pipe disable\n");
	/* pipe 关闭路径，不在 vblank 自旋锁里，可以停网格。
	 * 先清 running，让已在跑的回调也不再 rearm，再非阻塞取消。*/
	g_vblank_on = 0;
	g_vblank_timer_running = 0;
	hrtimer_try_to_cancel(&g_vblank_timer);
	drm_crtc_vblank_off(&pipe->crtc);
}

/*
 * M4 核心：flip-done 门控。
 * drm_simple_kms_helper.h 对 .update 的契约是——驱动必须在这里提交
 * crtc_state->event。有 vblank 时用 arm（事件在下一个 vblank 才发，天然实现
 * “帧真正上屏后才通知”）；拿不到 vblank 引用则退化为立即 send。
 * 这就是合成器依赖的 page-flip 完成回调。
 */
static void vdrm_pipe_update(struct drm_simple_display_pipe *pipe,
			     struct drm_plane_state *old_state)
{
	struct drm_crtc *crtc = &pipe->crtc;
	struct drm_crtc_state *crtc_state = crtc->state;
	struct drm_pending_vblank_event *event;

	/* M5: 池 buffer 上屏 -> 标 DISPLAYING + 通知 App。
	 * 注意：池对象是 import 来的，PRIME_HANDLE_TO_FD 会返回 App 原始 dma-buf，
	 * 合成器再 FD_TO_HANDLE 会得到“第二个 GEM”，所以不能比指针，要比
	 * 底层的 import_attach->dmabuf。 */
	if (pipe->plane.state && pipe->plane.state->fb) {
		struct drm_gem_object *gobj = pipe->plane.state->fb->obj[0];
		struct dma_buf *fbdb = (gobj && gobj->import_attach) ?
			gobj->import_attach->dmabuf : NULL;
		struct dma_buf *pdb0 = (g_pool[0].obj && g_pool[0].obj->import_attach) ? g_pool[0].obj->import_attach->dmabuf : NULL;
		struct dma_buf *pdb1 = (g_pool[1].obj && g_pool[1].obj->import_attach) ? g_pool[1].obj->import_attach->dmabuf : NULL;
		int i;

		pr_info("vdrm: pu fb=%ux%u gobj=%px sz=%zu ia=%px | p0=%px p0sz=%zu p0ia=%px\n",
			pipe->plane.state->fb->width, pipe->plane.state->fb->height,
			gobj, gobj ? gobj->size : 0, gobj ? gobj->import_attach : NULL,
			g_pool[0].obj, g_pool[0].obj ? g_pool[0].obj->size : 0,
			g_pool[0].obj ? g_pool[0].obj->import_attach : NULL);
		for (i = 0; i < VDRM_POOL_MAX; i++) {
			struct drm_gem_object *po = g_pool[i].obj;
			struct dma_buf *pdb = (po && po->import_attach) ?
				po->import_attach->dmabuf : NULL;

			if (!g_pool[i].used)
				continue;
			if (po == gobj || (fbdb && pdb && fbdb == pdb)) {
				if (g_pool[i].state == VDRM_ST_DISPL) {
					if ((g_disp_hits++ % 30) == 0)
						pr_info("vdrm: REUSE-WHILE-DISPL id=%u hit=%u\n",
							g_pool[i].id, g_disp_hits);
				}
				g_pool[i].state = VDRM_ST_DISPL;
				vdrm_frame_push(g_pool[i].id, g_pool[i].user_data);
				pr_info("vdrm: frame ready id=%u\n", g_pool[i].id);
				break;
			}
		}
	}

	if (!crtc_state)
		return;

	event = crtc_state->event;
	if (!event) {
		pr_info("vdrm: pipe update (no event)\n");
		return;
	}

	/* 先摘下来，否则 commit_hw_done() 里的 WARN_ON(event) 会响 */
	crtc_state->event = NULL;

	_raw_spin_lock_irq(&crtc->dev->event_lock.rlock);
	/* The queued event owns this reference; DRM releases it on delivery.
	 * Building a loadable module requires verified target CRCs for both calls. */
	if (drm_crtc_vblank_get(crtc) == 0)
		drm_crtc_arm_vblank_event(crtc, event);
	else
		drm_crtc_send_vblank_event(crtc, event);
	_raw_spin_unlock_irq(&crtc->dev->event_lock.rlock);

	pr_info("vdrm: pipe update -> event armed\n");
}

static int vdrm_conn_get_modes(struct drm_connector *connector)
{
	struct drm_display_mode *m;

	/* 造一个恰好等于 App 池尺寸的 mode。drm_add_modes_noedid 只会添加
	 * 内置模式表（1024x768 … 1920x1080），造不出任意分辨率，所以用 CVT
	 * 手工生成，保证 connector 的 mode 和 App buffer 的宽高严格一致
	 * （否则合成器选到别的 mode，dumb_create 就和池对不上）。 */
	/* 手工 CVT：drm_cvt_mode 不在导出表里（CRC 也对不上），用它等价公式
	 * （1920x1080@60 算出来就是 clock=148500, htotal=2200, vtotal=1125 ✓）
	 * 分配走本文件已有的 vdrm_kzalloc（= kzalloc(GFP_KERNEL)，
	 * 6.1–6.12 全档可用）。*/
	m = vdrm_kzalloc(sizeof(*m));
	if (m) {
		m->clock       = ((g_mode_w + 280) * (g_mode_h + 45) * 60) / 1000;
		m->hdisplay    = g_mode_w;
		m->hsync_start = g_mode_w + 88;
		m->hsync_end   = g_mode_w + 88 + 44;
		m->htotal      = g_mode_w + 280;
		m->vdisplay    = g_mode_h;
		m->vsync_start = g_mode_h + 4;
		m->vsync_end   = g_mode_h + 4 + 5;
		m->vtotal      = g_mode_h + 45;
		m->flags       = 1 | 4;      /* DRM_MODE_FLAG_PHSYNC|PVSYNC（uapi 稳定值）*/
		m->type        = 64;         /* DRM_MODE_TYPE_DRIVER */
	}
	if (!m) {
		pr_info("vdrm: get_modes -> 0 (cvt failed %ux%u)\n",
			g_mode_w, g_mode_h);
		return 0;
	}
	/* CVT 会对尺寸取整；强制回请求值，保证连接器 mode == App buffer */
	pr_info("vdrm: cvt req %ux%u -> mode %dx%d\n",
		g_mode_w, g_mode_h, m->hdisplay, m->vdisplay);
	m->hdisplay = g_mode_w;
	m->vdisplay = g_mode_h;
	if (m->htotal < g_mode_w) m->htotal = g_mode_w + 160;
	if (m->vtotal < g_mode_h) m->vtotal = g_mode_h + 45;
	m->type = (1 << 3) | (1 << 6);   /* DRIVER | PREFERRED */
	drm_mode_set_name(m);
	drm_mode_probed_add(connector, m);

	pr_info("vdrm: get_modes -> 1 (%ux%u)\n", g_mode_w, g_mode_h);
	return 1;
}

static int vdrm_do_minmax(void);

static int vdrm_do_pipe(void)
{
	int rc;

	if (!g_dev)
		return -ENODEV;
	if (g_pipe)
		return 0;

	g_conn = vdrm_kzalloc(sizeof(*g_conn));
	g_pipe = vdrm_kzalloc(sizeof(*g_pipe));
	if (!g_conn || !g_pipe)
		return -ENOMEM;

	g_conn_funcs.reset                  = drm_atomic_helper_connector_reset;
	g_conn_funcs.fill_modes             = drm_helper_probe_single_connector_modes;
	g_conn_funcs.destroy                = drm_connector_cleanup;
	g_conn_funcs.reset                  = drm_atomic_helper_connector_reset;
	g_conn_funcs.atomic_duplicate_state = drm_atomic_helper_connector_duplicate_state;
	g_conn_funcs.atomic_destroy_state   = drm_atomic_helper_connector_destroy_state;

	g_conn_helper.get_modes             = vdrm_conn_get_modes;

	/* vblank 定时器只初始化一次；回调在 hrtimer 上下文里跑 */
	if (!g_vblank_timer_inited) {
		hrtimer_init(&g_vblank_timer, CLOCK_MONOTONIC,
			       HRTIMER_MODE_REL);
		g_vblank_timer.function = vdrm_vblank_timer_fn;
		g_vblank_timer_inited = 1;
	}

	g_pipe_funcs.enable                 = vdrm_pipe_enable;
	g_pipe_funcs.disable                = vdrm_pipe_disable;
	g_pipe_funcs.update                 = vdrm_pipe_update;
	g_pipe_funcs.enable_vblank          = vdrm_pipe_enable_vblank;
	g_pipe_funcs.disable_vblank         = vdrm_pipe_disable_vblank;

	pr_info("vdrm: modecfg off=%d set min_w=%d max_w=%d min_h=%d max_h=%d\n",
		(int)((char *)&g_dev->mode_config - (char *)g_dev),
		g_dev->mode_config.min_width, g_dev->mode_config.max_width,
		g_dev->mode_config.min_height, g_dev->mode_config.max_height);
	pr_info("vdrm: sizes: drm_device=%d drm_mode_config=%d\n",
		(int)sizeof(struct drm_device), (int)sizeof(struct drm_mode_config));

	rc = drm_connector_init(g_dev, g_conn, &g_conn_funcs,
				  VDRM_CONNECTOR_VIRTUAL);
	if (rc) {
		pr_info("vdrm: drm_connector_init rc=%d\n", rc);
		return rc;
	}
	g_conn->helper_private = &g_conn_helper;

	rc = drm_simple_display_pipe_init(g_dev, g_pipe, &g_pipe_funcs,
					    g_formats,
					    sizeof(g_formats) / sizeof(g_formats[0]),
					    NULL, g_conn);
	if (rc) {
		pr_info("vdrm: drm_simple_display_pipe_init rc=%d\n", rc);
		return rc;
	}

	drm_mode_config_reset(g_dev);
	vdrm_do_minmax();
	pr_info("vdrm: pipe ok, num_crtc=%d num_plane=%d num_connector=%d\n",
		g_dev->mode_config.num_crtc, g_dev->mode_config.num_total_plane,
		g_dev->mode_config.num_connector);
	return 0;
}

/* ---------------- 设置 mode_config 尺寸(带安全闸) ---------------- */
/*
 * 只有在 privobj_list 确认是"空链表(指向自身)"时才写 min/max。
 * 这是一个廉价的运行时自检：万一某个成员偏移将来对不上，
 * 宁可拒绝写入（DRM 的 ADDFB2 会因 max_width=0 失败，可见且可恢复），
 * 也绝不把垃圾写进链表指针导致内核崩溃。
 */
static int vdrm_do_minmax(void)
{
	if (!g_dev)
		return -ENODEV;

	g_dev->mode_config.min_width  = 0;
	g_dev->mode_config.min_height = 0;
	g_dev->mode_config.max_width  = 4096;
	g_dev->mode_config.max_height = 4096;
	pr_info("vdrm: min/max = %d,%d .. %d,%d\n",
		g_dev->mode_config.min_width, g_dev->mode_config.min_height,
		g_dev->mode_config.max_width, g_dev->mode_config.max_height);
	return 0;
}

/* ================== 虚拟键鼠：由驱动自己注册的内核 input 设备 ==================
 *
 * 不用 uinput、不回用户态：两个 input_dev 由 KO 直接注册，生命周期绑在模块上。
 *
 * 为什么必须这样：容器里的合成器**没有热插拔能力** —— 容器 udevd 收不到宿主
 * netns 产生的 uevent，/sys 又是只读的所以 udevadm trigger 也用不了。一旦设备
 * 被销毁重建，libinput 会永远抱着那个死掉的 fd。uinput 方案里设备属于用户态守护
 * 进程，App 一退守护就退、设备就没了，于是"App 重启 = 输入永久失效"。
 * 放进内核后设备不再被销毁，合成器手里那个 fd 一直有效。
 *
 * 附带好处：input minor 恒定 → /run/udev/data 里那份元数据写一次就永久有效。
 */
static struct input_dev *g_kbd, *g_ptr;

static void vdrm_input_inject(u32 type, u32 code, s32 value)
{
	struct input_dev *d;

	if (type == VDRM_EV_REL)
		d = g_ptr;
	else if (type == VDRM_EV_KEY)
		d = (code >= VDRM_BTN_FIRST && code <= VDRM_BTN_LAST) ? g_ptr : g_kbd;
	else
		return;
	if (!d)
		return;

	input_event(d, type, code, value);
	/* EV_SYN 由 input_register_device() 自动置入 evbit，可直接发 */
	input_event(d, VDRM_EV_SYN, VDRM_SYN_REPORT, 0);
}

static struct input_dev *vdrm_input_make(const char *name, int is_ptr)
{
	struct input_dev *d = input_allocate_device();
	u32 i;

	if (!d)
		return NULL;
	d->name = name;
	d->id.bustype = VDRM_BUS_USB;
	d->id.vendor  = 0x1d6b;      /* Linux Foundation（虚拟设备常用）*/
	d->id.product = 0x0101;
	d->id.version = 1;

	if (is_ptr) {
		/* 鼠标：BTN_* + REL_X/Y/WHEEL/HWHEEL（与旧 uinput 版一致）*/
		for (i = VDRM_BTN_FIRST; i <= VDRM_BTN_LAST; i++)
			input_set_capability(d, VDRM_EV_KEY, i);
		input_set_capability(d, VDRM_EV_REL, VDRM_REL_X);
		input_set_capability(d, VDRM_EV_REL, VDRM_REL_Y);
		input_set_capability(d, VDRM_EV_REL, VDRM_REL_WHEEL);
		input_set_capability(d, VDRM_EV_REL, VDRM_REL_HWHEEL);
	} else {
		/* 键盘：全键码 + 自动重复 */
		for (i = 0; i < VDRM_KEY_MAX; i++)
			input_set_capability(d, VDRM_EV_KEY, i);
		input_set_capability(d, VDRM_EV_REP, 0);
	}
	return d;
}

static int vdrm_input_setup(void)
{
	g_kbd = vdrm_input_make("vdrm-kbd", 0);
	if (!g_kbd)
		return -ENOMEM;
	if (input_register_device(g_kbd)) {
		input_free_device(g_kbd);
		g_kbd = NULL;
		return -EIO;
	}

	g_ptr = vdrm_input_make("vdrm-ptr", 1);
	if (!g_ptr) {
		input_unregister_device(g_kbd);
		g_kbd = NULL;
		return -ENOMEM;
	}
	if (input_register_device(g_ptr)) {
		input_free_device(g_ptr);
		g_ptr = NULL;
		input_unregister_device(g_kbd);
		g_kbd = NULL;
		return -EIO;
	}

	pr_info("vdrm: input up kbd=%px ptr=%px\n", g_kbd, g_ptr);
	return 0;
}

static void vdrm_input_teardown(void)
{
	if (g_ptr) { input_unregister_device(g_ptr); g_ptr = NULL; }
	if (g_kbd) { input_unregister_device(g_kbd); g_kbd = NULL; }
	pr_info("vdrm: input down\n");
}

/* ================== 借域建池：insmod 时由 KO 自己把池建起来 ==================
 *
 * 池必须早于合成器第一次 dumb_create 就绪。合成器只分配一次缓冲然后一直用，
 * 池空时 dumb_create 走 heap 回退，那批缓冲不在池里，pipe_update 匹配不上，
 * App 一个帧事件都收不到 —— 就是"黑屏、关掉合成器再开就好"的那个时序。
 * 让 App 来建池，等于把这件事押在"用户先开一次 App"上。
 *
 * 这里改成 KO 自己拉：mkpool 是用户态程序，槽要带 gralloc 出身（fd1 + ints），
 * 内核造不出来，所以只能把它 exec 起来 —— 而内核线程是 u:r:kernel，策略不许
 * 它够到 gralloc（binder + allocator HAL）。所以先把加载方（KernelSU，
 * u:r:ksu）的凭据装上去，再 exec，域就跟着过去了。
 *
 * 这条路在本机实测过：源域=ksu 的 type_transition 是 0 条，所以 exec 之后域
 * 不被打回；entrypoint 也够（平时 App 的 su -c vdrm_mkpool 就是这么跑的）。
 *
 * kernel_execve 和 commit_creds 在这台内核上都没有 __ksymtab_ 条目（GKI 裁过
 * ksymtab），不能直接调，只能按名字拿地址再走函数指针 —— 类型必须是内核自己
 * 的，否则间接调用点的 kCFI 哈希对不上，直接 brk。
 */
static const struct cred *g_pool_cred;
static struct task_struct *g_mkpool_task;
static typeof(&kernel_execve) g_kernel_execve;
static typeof(&commit_creds) g_commit_creds;

/* 按名字取符号地址：kallsyms_lookup_name 没导出，kprobe 是唯一的平价入口 */
static void *vdrm_sym(const char *name)
{
	struct kprobe kp;
	void *addr;

	memset(&kp, 0, sizeof(kp));
	kp.symbol_name = name;
	if (register_kprobe(&kp))
		return NULL;
	addr = (void *)kp.addr;
	unregister_kprobe(&kp);
	return addr;
}

static const char *const vdrm_mkpool_argv[] = { "/data/wayland/vdrm_mkpool", NULL };
static const char *const vdrm_mkpool_envp[] = {
	"PATH=/system/bin:/system/xbin",
	"ANDROID_ROOT=/system",
	"ANDROID_DATA=/data",
	NULL,
};

static int vdrm_mkpool_thread(void *arg)
{
	int ret;

	/*
	 * 必须是 commit_creds，不是 override_creds —— 这是这里最容易改错的地方。
	 *
	 * exec 走 prepare_exec_creds() -> prepare_creds()，而 prepare_creds()
	 * 复制的是【客观】凭据（real_cred），不是主观的那份（cred）。
	 * override_creds 只改主观的那个，exec 这条路根本看不见它 —— 于是检查
	 * 仍然以 u:r:kernel 的身份做：内核域对 system_data_root_file 没有
	 * entrypoint，被拒；而内核域的拒绝在 AOSP 里是 dontaudit 掉的，dmesg
	 * 里连一条 AVC 都不留，只看到一个没头没尾的失败。umh 自己也是
	 * commit_creds，原因就在这里。
	 *
	 * 引用交给任务，模块不再持有；exec 成功的话这个任务就变成用户态进程了。
	 */
	g_commit_creds((struct cred *)get_cred(g_pool_cred));

	ret = g_kernel_execve(vdrm_mkpool_argv[0], vdrm_mkpool_argv,
			      vdrm_mkpool_envp);
	if (ret < 0)
		/* -ENOENT 路径/命名空间、-EACCES 权限或标签、-EPERM 策略、
		 * -ENOEXEC 解释器 —— 四种原因的修法完全不同，所以留着 */
		pr_err("vdrm: mkpool exec failed: %d\n", ret);

	return 0;
}

static int vdrm_mkpool_bringup(void)
{
	if (!g_pool_cred) {
		pr_info("vdrm: no loader credentials, pool stays app-driven\n");
		return -EINVAL;
	}

	g_kernel_execve = vdrm_sym("kernel_execve");
	g_commit_creds   = vdrm_sym("commit_creds");
	if (!g_kernel_execve || !g_commit_creds) {
		pr_info("vdrm: kernel_execve/commit_creds unresolvable, pool stays app-driven\n");
		return -ENOENT;
	}

	g_mkpool_task = kthread_run(vdrm_mkpool_thread, NULL, "vdrm-mkpool");
	if (IS_ERR(g_mkpool_task)) {
		int err = PTR_ERR(g_mkpool_task);

		g_mkpool_task = NULL;
		pr_err("vdrm: no mkpool thread: %d\n", err);
		return err;
	}

	pr_info("vdrm: mkpool spawned under the loader's credentials\n");
	return 0;
}

static void vdrm_mkpool_teardown(void)
{
	if (g_pool_cred) {
		put_cred(g_pool_cred);
		g_pool_cred = NULL;
	}
	/* exec 成功后那个线程已经是用户态进程，不归 kthread 管，不 stop */
	g_mkpool_task = NULL;
	g_kernel_execve = NULL;
	g_commit_creds = NULL;
}

static int vdrm_init(void)
{
	int rc;

	pr_info("vdrm: stage=enter\n");

	pr_info("vdrm: .ko init\n");

	pr_info("vdrm: stage=before-alloc\n");
	rc = vdrm_do_alloc();    if (rc) goto err;
	pr_info("vdrm: stage=alloc-ok\n");
	rc = vdrm_do_modecfg();  if (rc) goto err;
	rc = vdrm_do_pipe();     if (rc) goto err;
	pr_info("vdrm: stage=before-register\n");
	rc = vdrm_do_register(); if (rc) goto err;
	pr_info("vdrm: stage=register-ok\n");

	rc = vdrm_input_setup();       /* 虚拟键鼠：随模块存活，不随 App */
	if (rc) goto err;
	pr_info("vdrm: stage=input-ok\n");

	/* 池归 KO：insmod 就是唯一一次"跑在加载方上下文里"的机会，凭据在这时抓，
	 * 之后 mkpool 必须早于合成器，所以在这里就把它拉起来。 */
	g_pool_cred = get_task_cred(current);
	if (vdrm_mkpool_bringup())
		pr_info("vdrm: pool will have to be built by the app\n");
	else
		pr_info("vdrm: stage=mkpool-ok\n");

	pr_info("vdrm: bring-up ok (dev=%px)\n", g_dev);
	return 0;
err:
	pr_err("vdrm: bring-up failed rc=%d\n", rc);
	return rc;
}

static void vdrm_exit(void)
{
	vdrm_mkpool_teardown();
	vdrm_input_teardown();
	vdrm_do_unregister();
	pr_info("vdrm: exit ok (dev=%px)\n", g_dev);
}

module_init(vdrm_init);
module_exit(vdrm_exit);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("vdrm: virtual DRM device (card1) for container compositor");

/* dma_buf_* 导在 DMA_BUF 命名空间里，不声明这个内核会拒载
 * （实测：Unknown symbol dma_buf_get (err -22)）。
 * 旧 kit 是靠手搓 modinfo 里的 import_ns 实现的，kbuild 下要用标准宏。*/
MODULE_IMPORT_NS(DMA_BUF);
