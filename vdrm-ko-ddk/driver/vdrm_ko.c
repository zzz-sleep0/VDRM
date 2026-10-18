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
#include <linux/input/mt.h>	/* input_mt_init_slots / INPUT_MT_DIRECT */
#include <linux/dma-buf.h>
#include <linux/bitmap.h>
#include <linux/mm.h>
#include <linux/list.h>
#include <linux/fs.h>

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
#include <drm/drm_probe_helper.h>   /* drm_kms_helper_hotplug_event */
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

/* 诊断旋钮：为 1 时 pipe_update 直接 send_vblank_event（flip-done 立即完成），
 * 不 arm 到下一个 hrtimer vblank。虚拟显示没有真实扫描，这是更简单的等价路径；
 * 默认 0 保持原有 arm 行为，方便同一个会话内 A/B。*/
static bool g_flip_done_immediate = true;
module_param_named(flip_done_immediate, g_flip_done_immediate, bool, 0644);
MODULE_PARM_DESC(flip_done_immediate,
		 "Complete page-flip events immediately instead of at the next virtual vblank");

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

/* 批量注入：一次 ioctl 发多个键/相对位移，内核每个设备只补一个 SYN。
 * 指针移动的 X/Y、滚轮连发、修饰键组合都能合并，减少 syscall。*/
#define VDRM_INPUT_BATCH_MAX 16
struct vdrm_input_batch {
	u32 count;
	u32 flags;     /* reserved, must be 0 */
	struct vdrm_input_event events[VDRM_INPUT_BATCH_MAX];
};
#define VDRM_IOCTL_INPUT_BATCH \
	VDRM_IOC(1, 'V', 0x0b, sizeof(struct vdrm_input_batch))

/* ---- 直接触摸屏（ABS_MT）----
 * App 每帧把【当前所有活动触点】打包发过来；内核负责：
 *   - 上一帧有、这一帧消失的 slot 自动发 TRACKING_ID=-1（抬起）
 *   - BTN_TOUCH 只在 0↔1 变化时发
 *   - 整帧结束只发一个 SYN_REPORT（MT 协议 B 不能每个事件一个 SYN）
 * 坐标用归一化的 0..4095，跟输出分辨率解耦；App 负责把窗口坐标
 * 按 letterbox 逆映射到 mode 后再归一化。*/
#define VDRM_TOUCH_MAX        10
#define VDRM_TOUCH_COORD_MAX  4095

struct vdrm_touch_point {
	s32 slot;      /* 0..VDRM_TOUCH_MAX-1 */
	s32 tracking;  /* 触点身份，>=0；同一次触摸保持不变 */
	s32 x, y;      /* 0..VDRM_TOUCH_COORD_MAX */
	s32 pressure;  /* 0..255 */
};
struct vdrm_touch_frame {
	u32 count;     /* 本帧活动触点数；0 = 全部抬起 */
	u32 flags;     /* reserved, must be 0 */
	struct vdrm_touch_point points[VDRM_TOUCH_MAX];
};
#define VDRM_IOCTL_TOUCH       VDRM_IOC(1, 'V', 0x09, sizeof(struct vdrm_touch_frame))

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

static struct vdrm_pool_entry g_pool[VDRM_POOL_MAX];
static struct vdrm_pool_list  g_pool_list;   /* GET_POOL 用（避免内核栈上放 512B）*/
static int                 g_pool_next_id = 1;
static struct mutex        g_pool_lock;        /* 静态清零 = 已解锁 */
/* 只保护池槽的 used/state/obj 等少量字段，供提交路径（不能睡眠）读取。
 * 大分配/导入仍由 g_pool_lock 串行化。*/
static DEFINE_SPINLOCK(g_pool_state_lock);
static struct miscdevice   g_misc;
static struct file_operations g_misc_fops;
static int                 g_misc_registered;
/* /dev/vdrm 的打开计数：只有最后一个使用者关闭时，才补发输入释放、
 * 清帧队列、把 DISPL 槽还给池。mkpool 的短暂 open/close 不会打断 App。*/
static atomic_t            g_misc_opens = ATOMIC_INIT(0);

/* 输出几何（唯一 connector 的 preferred mode，XRGB8888）。
 *
 * 默认 1920x1080；加载时可以用 `insmod vdrm.ko mode=2376x1080` 覆盖。
 * 改尺寸不用重编模块，但仍然要重载：池、mkpool、App 的 AHB 都从
 * GET_INFO 拿尺寸，跟着这条 mode 走；连接器只有这一条 preferred 模式。*/
static u32 g_mode_w = 1920, g_mode_h = 1080, g_mode_pitch = 1920 * 4;
static char *g_mode_param;
module_param_named(mode, g_mode_param, charp, 0444);
MODULE_PARM_DESC(mode, "Virtual output mode WxH (default 1920x1080)");

/* 虚拟输出的刷新率（Hz）。vblank 周期由 mode 的 clock/htotal/vtotal 反推，
 * 所以改这里就够了：60 时 niri 出 60fps，120 时出 120fps（宿主面板是 120
 * 就对齐到 120）。默认 120，可 insmod vdrm.ko refresh=60 覆盖。 */
static uint g_refresh = 120;
module_param_named(refresh, g_refresh, uint, 0444);
MODULE_PARM_DESC(refresh, "Virtual output refresh rate in Hz (24..240, default 120)");

/* 解析 module_param mode。范围与 mode_config 的 max 一致（4096），
 * 下限取 16 —— 再小 CVT 的 porch 都放不下。 */
static int vdrm_parse_mode_param(void)
{
	unsigned int w, h;
	char tail;

	if (g_refresh < 24 || g_refresh > 240) {
		pr_err("vdrm: refresh %u out of range (24..240)\n", g_refresh);
		return -ERANGE;
	}

	if (!g_mode_param || !*g_mode_param)
		return 0;

	/* 多一个字符就说明不是严格的 WxH（例如 1920x1080@60 会被拒绝）*/
	if (sscanf(g_mode_param, "%ux%u%c", &w, &h, &tail) != 2) {
		pr_err("vdrm: bad mode '%s' (expect WxH)\n", g_mode_param);
		return -EINVAL;
	}
	if (w < 16 || h < 16 || w > 4096 || h > 4096) {
		pr_err("vdrm: mode %ux%u out of range (16..4096)\n", w, h);
		return -ERANGE;
	}

	g_mode_w     = w;
	g_mode_h     = h;
	g_mode_pitch = ALIGN(w * 4, 64);
	pr_info("vdrm: mode param -> %ux%u pitch=%u\n",
		g_mode_w, g_mode_h, g_mode_pitch);
	return 0;
}

/* ---- 帧就绪通知：只保留最新一帧（App 本来就是 latest-wins）---- */
struct vdrm_frame_event {
	u32 id;
	u32 sequence;
	u64 timestamp_ns;
	u64 user_data;
};
static struct vdrm_frame_event g_frame_latest;
static bool                   g_frame_has;
static u32                    g_frame_seq;
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
	return ret;
}

/*
 * dumb_create：优先从池里发 App 的 gralloc buffer（伪装成 card1 原生 dumb），
 * 池里没有匹配/空闲的，回退到自持 shmem（合成器永不卡死）。
 */
/* 只从池里发缓冲；池由 mkpool 在 boot 时建好。没有匹配/空闲槽时
 * 直接 -ENOSPC —— 不再保留第二套 heap 出身（那批 buffer 没有原始
 * fd0/fd1，App 根本 forge 不出来，只会变成不可显示的隐藏状态）。*/
static int vdrm_dumb_create(struct drm_file *file, struct drm_device *dev,
			    struct drm_mode_create_dumb *args)
{
	int i;

	(void)dev;
	if (args->bpp != 32)
		return -EINVAL;

	mutex_lock(&g_pool_lock);
	for (i = 0; i < VDRM_POOL_MAX; i++) {
		struct vdrm_pool_entry *e = &g_pool[i];
		u32 handle;
		int rc;

		if (!e->used || e->allocation_reserved || e->state != VDRM_ST_FREE)
			continue;
		if (!e->f0)
			continue;
		if (e->width != args->width || e->height != args->height)
			continue;

		rc = drm_gem_handle_create(file, e->obj, &handle);
		if (rc)
			continue;
		e->allocation_reserved = 1;
		e->allocation_owner = file;
		e->state = VDRM_ST_TAKEN;
		args->handle = handle;
		args->pitch  = e->pitch;
		args->size   = e->obj->size;
		mutex_unlock(&g_pool_lock);
		pr_debug("vdrm: dumb_create -> POOL id=%u handle=%u %ux%u pitch=%u\n",
			e->id, handle, e->width, e->height, e->pitch);
		return 0;
	}
	mutex_unlock(&g_pool_lock);
	return -ENOSPC;
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
	pr_debug("vdrm: dumb_map_offset handle=%u obj=%px rc=%d off=%llx ia=%px db=%px\n",
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

	ev.id           = id;
	ev.user_data    = user_data;
	ev.timestamp_ns = (u64)ktime_get();

	/* pipe_update 上下文不能睡；新帧直接覆盖旧帧。*/
	flags = _raw_spin_lock_irqsave(&g_frame_lock.rlock);
	ev.sequence     = ++g_frame_seq;
	g_frame_latest  = ev;
	g_frame_has     = true;
	_raw_spin_unlock_irqrestore(&g_frame_lock.rlock, flags);

	__wake_up(&g_frame_wq, 3 /* TASK_NORMAL */, 1, NULL);
}

static int vdrm_frame_pop(struct vdrm_frame_event *ev)
{
	unsigned long flags;

	flags = _raw_spin_lock_irqsave(&g_frame_lock.rlock);
	if (!g_frame_has) {
		_raw_spin_unlock_irqrestore(&g_frame_lock.rlock, flags);
		return -1;
	}
	*ev = g_frame_latest;
	g_frame_has = false;
	_raw_spin_unlock_irqrestore(&g_frame_lock.rlock, flags);
	return 0;
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
	nonempty = g_frame_has;
	_raw_spin_unlock_irqrestore(&g_frame_lock.rlock, flags);

	if (nonempty)
		mask |= 0x0001 | 0x0040;   /* POLLIN | POLLRDNORM */
	return mask;
}

/* App 关闭 /dev/vdrm（或进程被杀）时内核回调。
 *
 * 池归模块所有，不在这里清；但最后一个使用者消失时必须做三件事，
 * 否则下一次会话会继承脏状态：
 *   1. 补发按键抬起 / 所有触点 lift —— 进程被强杀时容器里不会留下
 *      “一直按着的键或手指”；
 *   2. 清帧事件环 —— 新 App 不读上一实例的旧事件；
 *   3. DISPL 槽回到 FREE —— 没有消费者了，没人会再替它们 RETURN。
 * 只有最后一个 open 关闭时才做，mkpool 的短暂 open/close 不会误伤
 * 正在工作的 App。 */
static void vdrm_input_reset_state(void);   /* 定义见文件后部 */

static int vdrm_misc_release(struct inode *inode, struct file *file)
{
	bool last;

	(void)inode;
	(void)file;

	last = atomic_dec_and_test(&g_misc_opens);
	pr_info("vdrm: misc release (last=%d, pool kept)\n", last);
	if (!last)
		return 0;

	vdrm_input_reset_state();

	/* 清最新帧：否则新 App 会读到上一实例遗留的旧事件 */
	g_frame_has = false;
	g_frame_seq = 0;

	mutex_lock(&g_pool_lock);
	{
		unsigned long flags;
		int i;

		spin_lock_irqsave(&g_pool_state_lock, flags);
		for (i = 0; i < VDRM_POOL_MAX; i++) {
			if (g_pool[i].used && g_pool[i].state == VDRM_ST_DISPL)
				g_pool[i].state = VDRM_ST_FREE;
		}
		spin_unlock_irqrestore(&g_pool_state_lock, flags);
	}
	mutex_unlock(&g_pool_lock);

	return 0;
}

static void vdrm_input_inject(u32 type, u32 code, s32 value);   /* 定义见文件后部 */
static int vdrm_input_inject_batch(const struct vdrm_input_batch *b);
static int vdrm_touch_inject(const struct vdrm_touch_frame *f); /* 定义见文件后部 */

static int vdrm_misc_open(struct inode *inode, struct file *file)
{
	(void)inode;
	(void)file;
	atomic_inc(&g_misc_opens);
	return 0;
}

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
		pr_debug("vdrm: GET_INFO sizeof=%d abi=%u %ux%u fmt=%08x pitch=%u cap=%u arg=%px\n",
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
		pr_debug("vdrm: GET_POOL -> %d entries\n", n);
		if (copy_to_user((void *)arg, &g_pool_list,
				      sizeof(g_pool_list)))
			return -EFAULT;
		return 0;
	}
	case VDRM_IOCTL_REGISTER: {
		struct vdrm_register_req req;
		struct dma_buf *dbuf;
		struct drm_gem_object *obj;
		unsigned long flags;
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
		/* 所有字段就绪后再发布 used，提交路径不会看到半成品。*/
		spin_lock_irqsave(&g_pool_state_lock, flags);
		g_pool[slot].used      = 1;
		spin_unlock_irqrestore(&g_pool_state_lock, flags);
		mutex_unlock(&g_pool_lock);

		pr_info("vdrm: REGISTER id=%u %ux%u pitch=%u size=%zu\n",
			req.id, req.width, req.height, req.pitch, obj->size);
		if (copy_to_user((void *)arg, &req, sizeof(req))) {
			/* 回传失败：把已入池的 slot 退掉 */
			mutex_lock(&g_pool_lock);
			spin_lock_irqsave(&g_pool_state_lock, flags);
			g_pool[slot].used = 0;
			spin_unlock_irqrestore(&g_pool_state_lock, flags);
			memset(&g_pool[slot], 0, sizeof(g_pool[slot]));
			mutex_unlock(&g_pool_lock);
			vdrm_gem_put(obj);
			return -EFAULT;
		}

		return 0;
	}
	case VDRM_IOCTL_RETURN: {
		struct vdrm_handle_req req;
		unsigned long flags;
		int i, rc = -ENOENT;

		if (copy_from_user(&req, (const void *)arg, sizeof(req)))
			return -EFAULT;
		mutex_lock(&g_pool_lock);
		spin_lock_irqsave(&g_pool_state_lock, flags);
		for (i = 0; i < VDRM_POOL_MAX; i++) {
			if (g_pool[i].used && g_pool[i].id == req.id) {
				if (g_pool[i].state == VDRM_ST_DISPL)
					g_pool[i].state = VDRM_ST_FREE;
				rc = 0;
				break;
			}
		}
		spin_unlock_irqrestore(&g_pool_state_lock, flags);
		mutex_unlock(&g_pool_lock);
		return rc;
	}
	case VDRM_IOCTL_UNREGISTER: {
		struct vdrm_handle_req req;
		struct drm_gem_object *obj = NULL;
		struct file *f0 = NULL, *f1 = NULL;
		unsigned long flags;
		int i;

		if (copy_from_user(&req, (const void *)arg, sizeof(req)))
			return -EFAULT;
		mutex_lock(&g_pool_lock);

		spin_lock_irqsave(&g_pool_state_lock, flags);
		for (i = 0; i < VDRM_POOL_MAX; i++) {
			if (g_pool[i].used && (!req.id || g_pool[i].id == req.id) &&
			    (g_pool[i].allocation_reserved || g_pool[i].state != VDRM_ST_FREE)) {
				spin_unlock_irqrestore(&g_pool_state_lock, flags);
				mutex_unlock(&g_pool_lock);
				return -EBUSY;
			}
		}
		spin_unlock_irqrestore(&g_pool_state_lock, flags);

		if (req.id == 0) {
			/* id=0 = 清空整池（mkpool 开跑前用，避免多次运行累积批次）*/
			for (i = 0; i < VDRM_POOL_MAX; i++) {
				bool used;

				spin_lock_irqsave(&g_pool_state_lock, flags);
				used = g_pool[i].used;
				if (used)
					g_pool[i].used = 0;
				spin_unlock_irqrestore(&g_pool_state_lock, flags);
				if (!used)
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

		spin_lock_irqsave(&g_pool_state_lock, flags);
		for (i = 0; i < VDRM_POOL_MAX; i++) {
			if (g_pool[i].used && g_pool[i].id == req.id) {
				g_pool[i].used = 0;
				obj = g_pool[i].obj;
				f0 = g_pool[i].f0;
				f1 = g_pool[i].f1;
				break;
			}
		}
		spin_unlock_irqrestore(&g_pool_state_lock, flags);
		if (!obj) {
			mutex_unlock(&g_pool_lock);
			return -ENOENT;
		}
		memset(&g_pool[i], 0, sizeof(g_pool[i]));
		mutex_unlock(&g_pool_lock);
		if (f0) fput(f0);
		if (f1) fput(f1);
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
	case VDRM_IOCTL_INPUT_BATCH: {
		struct vdrm_input_batch batch;

		if (copy_from_user(&batch, (const void *)arg, sizeof(batch)))
			return -EFAULT;
		if (batch.flags)
			return -EINVAL;
		return vdrm_input_inject_batch(&batch);
	}
	case VDRM_IOCTL_TOUCH: {
		struct vdrm_touch_frame f;

		if (copy_from_user(&f, (const void *)arg, sizeof(f)))
			return -EFAULT;
		if (f.flags)
			return -EINVAL;
		return vdrm_touch_inject(&f);
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
static void vdrm_postclose(struct drm_device *dev, struct drm_file *file)
{
	unsigned long flags;
	int i;
	(void)dev;

	mutex_lock(&g_pool_lock);
	spin_lock_irqsave(&g_pool_state_lock, flags);
	for (i = 0; i < VDRM_POOL_MAX; i++) {
		struct vdrm_pool_entry *e = &g_pool[i];

		if (!e->used || e->allocation_owner != file)
			continue;
		/* 归属归还；显示占用仍由 App 的 RETURN 决定（DISPL 保持不动） */
		e->allocation_reserved = 0;
		e->allocation_owner    = NULL;
		if (e->state == VDRM_ST_TAKEN)
			e->state = VDRM_ST_FREE;
	}
	spin_unlock_irqrestore(&g_pool_state_lock, flags);
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
	g_mode_funcs.atomic_check  = drm_atomic_helper_check;
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
		g_misc_fops.open           = vdrm_misc_open;
		g_misc_fops.unlocked_ioctl = vdrm_misc_ioctl;
		g_misc_fops.compat_ioctl   = vdrm_misc_ioctl;
		g_misc_fops.read           = vdrm_misc_read;
		g_misc_fops.poll           = vdrm_misc_poll;
		g_misc_fops.release        = vdrm_misc_release;
		/* 初始化帧等待队列（静态清零的 spinlock = 解锁）*/
		g_frame_wq.head.next = &g_frame_wq.head;
		g_frame_wq.head.prev = &g_frame_wq.head;
		g_frame_has = false;
		g_misc.minor = 255;        /* MISC_DYNAMIC_MINOR */
		g_misc.name  = "vdrm";
		g_misc.fops  = &g_misc_fops;
		/* 默认只给 root；service.sh 会把它 chown 给 VDRM App 的 uid
		 * 并保持 0600。这样普通 App 不能注入输入或取池 fd。*/
		g_misc.mode  = 0600;
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

	/* 网格连续前进：没人持 vblank 引用时也照常走，只是不上报。*/
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
	/* 只在网格还没跑起来时启动；之后相位不被打断。*/
	if (!g_vblank_timer_running) {
		g_vblank_timer_running = 1;
		hrtimer_start_range_ns(&g_vblank_timer,
				       (ktime_t)g_vblank_period_ns,
				       0, HRTIMER_MODE_REL);
	}
	pr_debug("vdrm: vblank on (period=%llu ns)\n",
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
	pr_debug("vdrm: vblank off\n");
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
		u32 ready_id = 0;
		u64 ready_ud = 0;
		bool ready = false;
		unsigned long flags;
		int i;

		spin_lock_irqsave(&g_pool_state_lock, flags);
		for (i = 0; i < VDRM_POOL_MAX; i++) {
			struct drm_gem_object *po = g_pool[i].obj;
			struct dma_buf *pdb = (po && po->import_attach) ?
				po->import_attach->dmabuf : NULL;

			if (!g_pool[i].used)
				continue;
			if (po == gobj || (fbdb && pdb && fbdb == pdb)) {
				g_pool[i].state = VDRM_ST_DISPL;
				ready_id = g_pool[i].id;
				ready_ud = g_pool[i].user_data;
				ready = true;
				break;
			}
		}
		spin_unlock_irqrestore(&g_pool_state_lock, flags);

		if (ready) {
			vdrm_frame_push(ready_id, ready_ud);
			pr_debug("vdrm: frame ready id=%u\n", ready_id);
		}
	}

	if (!crtc_state)
		return;

	event = crtc_state->event;
	if (!event) {
		pr_debug("vdrm: pipe update (no event)\n");
		return;
	}

	/* 先摘下来，否则 commit_hw_done() 里的 WARN_ON(event) 会响 */
	crtc_state->event = NULL;

	_raw_spin_lock_irq(&crtc->dev->event_lock.rlock);
	/* 正常路径：arm 到下一个 vblank；诊断路径：立即完成。
	 * 立即路径不取 vblank 引用，事件由 DRM 在 send 时释放。*/
	if (READ_ONCE(g_flip_done_immediate) ||
	    drm_crtc_vblank_get(crtc) != 0)
		drm_crtc_send_vblank_event(crtc, event);
	else
		drm_crtc_arm_vblank_event(crtc, event);
	_raw_spin_unlock_irq(&crtc->dev->event_lock.rlock);

	pr_debug("vdrm: pipe update -> event %s\n",
		 READ_ONCE(g_flip_done_immediate) ? "sent" : "armed");
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
		m->clock       = ((g_mode_w + 280) * (g_mode_h + 45) * g_refresh) / 1000;
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
	pr_info("vdrm: cvt req %ux%u@%u -> mode %dx%d\n",
		g_mode_w, g_mode_h, g_refresh, m->hdisplay, m->vdisplay);
	m->hdisplay = g_mode_w;
	m->vdisplay = g_mode_h;
	if (m->htotal < g_mode_w) m->htotal = g_mode_w + 160;
	if (m->vtotal < g_mode_h) m->vtotal = g_mode_h + 45;
	m->type = (1 << 3) | (1 << 6);   /* DRIVER | PREFERRED */
	drm_mode_set_name(m);
	drm_mode_probed_add(connector, m);

	pr_info("vdrm: get_modes -> 1 (%ux%u@%u)\n", g_mode_w, g_mode_h, g_refresh);
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
static struct input_dev *g_kbd, *g_ptr, *g_touch;
/* 触摸屏上一帧状态（只由 /dev/vdrm ioctl 上下文读写） */
static u32 g_touch_slots;                       /* 活动 slot 位图 */
static s32 g_touch_tid[VDRM_TOUCH_MAX];         /* 各 slot 当前 tracking id */
/* 已注入、尚未抬起的按键。进程被强杀时靠它补发 key-up。 */
static DECLARE_BITMAP(g_kbd_down, VDRM_KEY_MAX + 1);
static DECLARE_BITMAP(g_ptr_down, VDRM_BTN_LAST + 1);
static DEFINE_MUTEX(g_input_lock);

static struct input_dev *vdrm_touch_make(void)
{
	struct input_dev *d = input_allocate_device();
	int i;

	if (!d)
		return NULL;
	d->name = "vdrm-touch";
	d->id.bustype = VDRM_BUS_USB;
	d->id.vendor  = 0x1d6b;
	d->id.product = 0x0102;
	d->id.version = 1;

	input_set_abs_params(d, ABS_MT_POSITION_X, 0, VDRM_TOUCH_COORD_MAX, 0, 0);
	input_set_abs_params(d, ABS_MT_POSITION_Y, 0, VDRM_TOUCH_COORD_MAX, 0, 0);
	input_set_abs_params(d, ABS_MT_PRESSURE, 0, 255, 0, 0);
	input_set_capability(d, EV_KEY, BTN_TOUCH);
	/* INPUT_MT_DIRECT 会设置 INPUT_PROP_DIRECT —— libinput 据此当触摸屏 */
	if (input_mt_init_slots(d, VDRM_TOUCH_MAX, INPUT_MT_DIRECT)) {
		input_free_device(d);
		return NULL;
	}
	for (i = 0; i < VDRM_TOUCH_MAX; i++)
		g_touch_tid[i] = -1;
	g_touch_slots = 0;
	return d;
}

/* 整帧注入：自动 lift 消失的 slot，只发一个 SYN_REPORT */
static int vdrm_touch_inject(const struct vdrm_touch_frame *f)
{
	struct input_dev *d = g_touch;
	u32 newmask = 0, oldmask;
	u32 i;
	int s;

	if (!d)
		return -ENODEV;
	if (f->count > VDRM_TOUCH_MAX)
		return -EINVAL;

	for (i = 0; i < f->count; i++) {
		const struct vdrm_touch_point *p = &f->points[i];

		if (p->slot < 0 || p->slot >= VDRM_TOUCH_MAX ||
		    p->tracking < 0 || (newmask & BIT(p->slot)))
			return -EINVAL;
		if (p->x < 0 || p->x > VDRM_TOUCH_COORD_MAX ||
		    p->y < 0 || p->y > VDRM_TOUCH_COORD_MAX ||
		    p->pressure < 0 || p->pressure > 255)
			return -EINVAL;
		newmask |= BIT(p->slot);
	}

	mutex_lock(&g_input_lock);
	oldmask = g_touch_slots;

	for (s = 0; s < VDRM_TOUCH_MAX; s++) {
		if ((oldmask & BIT(s)) && !(newmask & BIT(s))) {
			input_event(d, EV_ABS, ABS_MT_SLOT, s);
			input_event(d, EV_ABS, ABS_MT_TRACKING_ID, -1);
			g_touch_tid[s] = -1;
		}
	}

	for (i = 0; i < f->count; i++) {
		const struct vdrm_touch_point *p = &f->points[i];

		input_event(d, EV_ABS, ABS_MT_SLOT, p->slot);
		if (!(oldmask & BIT(p->slot)) ||
		    g_touch_tid[p->slot] != p->tracking) {
			input_event(d, EV_ABS, ABS_MT_TRACKING_ID, p->tracking);
			g_touch_tid[p->slot] = p->tracking;
		}
		input_event(d, EV_ABS, ABS_MT_POSITION_X, p->x);
		input_event(d, EV_ABS, ABS_MT_POSITION_Y, p->y);
		input_event(d, EV_ABS, ABS_MT_PRESSURE, p->pressure);
	}

	if (newmask && !oldmask)
		input_event(d, EV_KEY, BTN_TOUCH, 1);
	else if (!newmask && oldmask)
		input_event(d, EV_KEY, BTN_TOUCH, 0);

	input_event(d, EV_SYN, VDRM_SYN_REPORT, 0);
	g_touch_slots = newmask;
	mutex_unlock(&g_input_lock);
	return 0;
}

static struct input_dev *vdrm_input_dev_for(u32 type, u32 code)
{
	if (type == VDRM_EV_REL)
		return g_ptr;
	if (type == VDRM_EV_KEY)
		return (code >= VDRM_BTN_FIRST && code <= VDRM_BTN_LAST) ?
			g_ptr : g_kbd;
	return NULL;
}

/* caller holds g_input_lock */
static void vdrm_input_emit_locked(struct input_dev *d, u32 type, u32 code,
				   s32 value)
{
	if (type == VDRM_EV_KEY) {
		if (d == g_ptr && code <= VDRM_BTN_LAST) {
			if (value)
				__set_bit(code, g_ptr_down);
			else
				__clear_bit(code, g_ptr_down);
		} else if (d == g_kbd && code <= VDRM_KEY_MAX) {
			if (value)
				__set_bit(code, g_kbd_down);
			else
				__clear_bit(code, g_kbd_down);
		}
	}
	input_event(d, type, code, value);
}

static void vdrm_input_inject(u32 type, u32 code, s32 value)
{
	struct input_dev *d = vdrm_input_dev_for(type, code);

	if (!d)
		return;

	mutex_lock(&g_input_lock);
	vdrm_input_emit_locked(d, type, code, value);
	/* EV_SYN 由 input_register_device() 自动置入 evbit，可直接发 */
	input_event(d, VDRM_EV_SYN, VDRM_SYN_REPORT, 0);
	mutex_unlock(&g_input_lock);
}

/* 一次 ioctl 注入多个事件；每个设备只补一个 SYN_REPORT。*/
static int vdrm_input_inject_batch(const struct vdrm_input_batch *b)
{
	bool kbd = false, ptr = false;
	u32 i;

	if (b->count > VDRM_INPUT_BATCH_MAX)
		return -EINVAL;

	mutex_lock(&g_input_lock);
	for (i = 0; i < b->count; i++) {
		const struct vdrm_input_event *e = &b->events[i];
		struct input_dev *d = vdrm_input_dev_for(e->type, e->code);

		if (!d)
			continue;
		vdrm_input_emit_locked(d, e->type, e->code, e->value);
		if (d == g_kbd)
			kbd = true;
		else
			ptr = true;
	}
	if (kbd)
		input_event(g_kbd, VDRM_EV_SYN, VDRM_SYN_REPORT, 0);
	if (ptr)
		input_event(g_ptr, VDRM_EV_SYN, VDRM_SYN_REPORT, 0);
	mutex_unlock(&g_input_lock);
	return 0;
}

/* 最后一个 /dev/vdrm 使用者消失：把内核里仍按着的键和触点全部释放。
 * 进程被 LMK/强杀时这是唯一能通知容器 input 栈的时机。 */
static void vdrm_input_reset_state(void)
{
	unsigned long code;
	bool kbd_sent = false, ptr_sent = false;

	mutex_lock(&g_input_lock);

	if (g_kbd) {
		for_each_set_bit(code, g_kbd_down, VDRM_KEY_MAX + 1) {
			input_event(g_kbd, VDRM_EV_KEY, code, 0);
			kbd_sent = true;
		}
		bitmap_zero(g_kbd_down, VDRM_KEY_MAX + 1);
	}
	if (kbd_sent)
		input_event(g_kbd, VDRM_EV_SYN, VDRM_SYN_REPORT, 0);

	if (g_ptr) {
		for_each_set_bit(code, g_ptr_down, VDRM_BTN_LAST + 1) {
			input_event(g_ptr, VDRM_EV_KEY, code, 0);
			ptr_sent = true;
		}
		bitmap_zero(g_ptr_down, VDRM_BTN_LAST + 1);
	}
	if (ptr_sent)
		input_event(g_ptr, VDRM_EV_SYN, VDRM_SYN_REPORT, 0);

	if (g_touch && g_touch_slots) {
		int s;

		for (s = 0; s < VDRM_TOUCH_MAX; s++) {
			if (!(g_touch_slots & BIT(s)))
				continue;
			input_event(g_touch, EV_ABS, ABS_MT_SLOT, s);
			input_event(g_touch, EV_ABS, ABS_MT_TRACKING_ID, -1);
			g_touch_tid[s] = -1;
		}
		input_event(g_touch, EV_KEY, BTN_TOUCH, 0);
		input_event(g_touch, EV_SYN, VDRM_SYN_REPORT, 0);
		g_touch_slots = 0;
	}

	mutex_unlock(&g_input_lock);
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

	g_touch = vdrm_touch_make();
	if (!g_touch) {
		input_unregister_device(g_ptr);
		g_ptr = NULL;
		input_unregister_device(g_kbd);
		g_kbd = NULL;
		return -ENOMEM;
	}
	if (input_register_device(g_touch)) {
		input_free_device(g_touch);
		g_touch = NULL;
		input_unregister_device(g_ptr);
		g_ptr = NULL;
		input_unregister_device(g_kbd);
		g_kbd = NULL;
		return -EIO;
	}

	pr_info("vdrm: input up kbd=%px ptr=%px touch=%px\n",
		g_kbd, g_ptr, g_touch);
	return 0;
}

static void vdrm_input_teardown(void)
{
	/* 卸载前把仍按着的键/触点放掉，别把状态留给 input 核心 */
	vdrm_input_reset_state();
	if (g_touch) { input_unregister_device(g_touch); g_touch = NULL; }
	if (g_ptr) { input_unregister_device(g_ptr); g_ptr = NULL; }
	if (g_kbd) { input_unregister_device(g_kbd); g_kbd = NULL; }
	pr_info("vdrm: input down\n");
}

/* ===================== 池不由内核建 =====================
 *
 * 池必须早于合成器第一次 dumb_create 就绪：合成器只分配一次缓冲然后一直用，
 * 池空时 dumb_create 走 heap 回退，那批缓冲不在池里，pipe_update 匹配不上，
 * App 一个帧事件都收不到 —— 就是"黑屏、关掉合成器再开就好"的那个时序。
 *
 * 而池里的槽必须带 gralloc 出身（fd0 + fd1 metadata blob + ints），内核造不出来：
 * gralloc 分配是 binder 到 allocator HAL 的事。所以"建池"这一步天然属于用户态。
 *
 * 曾经试过让 KO 在 insmod 时自己把它拉起来：借加载方的 ksu 域（get_task_cred +
 * commit_creds）→ user_mode_thread（内核线程不能 exec）→ chroot /proc/1/root
 * （内核线程活在初始 mount ns，那里没有 statsd APEX）。能跑，但那是拿一百多行
 * 最脆的内核代码去伪造一个用户态上下文，还得按名字 kprobe 解析三个没导出的符号。
 *
 * 现在换成模块脚本：installer 的 service.sh 在 insmod 之后直接跑 vdrm_mkpool ——
 * 那个 shell 本来就是 root、就在正确的域和挂载树里，上面前三样绕法一样都不需要。
 *
 * 所以这里什么都不做：池由 helper（模块脚本跑的，或 App 的 ensure_pool_spawn
 * 兜底拉起的）经 VDRM_IOCTL_REGISTER 注册进来，内核只负责持有、匹配、清理。
 */

static int vdrm_init(void)
{
	int rc;

	pr_info("vdrm: stage=enter\n");

	/* 必须在 alloc/modecfg 之前：connector 的 get_modes、GET_INFO 都读它 */
	rc = vdrm_parse_mode_param();
	if (rc)
		return rc;

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

	/* 池不在这里建，见上面"池不由内核建" */
	pr_info("vdrm: bring-up ok (dev=%px)\n", g_dev);
	return 0;
err:
	pr_err("vdrm: bring-up failed rc=%d\n", rc);
	return rc;
}

static void vdrm_exit(void)
{
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
