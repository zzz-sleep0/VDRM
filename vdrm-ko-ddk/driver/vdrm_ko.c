/*
 * vdrm.c — 虚拟 DRM 设备 (card1)
 *
 * 一个普通的可加载模块：insmod 进来就建 card1 + /dev/vdrm + 三个虚拟输入设备，
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
#include <linux/eventfd.h>
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
/* 5.10 还没有这个头（prepare_fb 的实现那时叫 drm_gem_fb_prepare_fb，
 * 声明在 drm_gem_framebuffer_helper.h 里）。5.15 起才有。*/
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 15, 0)
#include <drm/drm_gem_atomic_helper.h>
#endif
#include <drm/drm_gem_framebuffer_helper.h>
#include <drm/drm_framebuffer.h>
#include <drm/drm_crtc.h>
#include <drm/drm_connector.h>
#include <drm/drm_edid.h>
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









/* 有了内核头，这些不再需要手抄数值 —— 名字保留，省得改动调用点。
 *
 * DRIVER_SYNCOBJ / DRIVER_SYNCOBJ_TIMELINE：这两个必须声明。
 * 它们是 DRM_CAP_SYNCOBJ / DRM_CAP_SYNCOBJ_TIMELINE 的来源
 * （drm_ioctl.c: drm_core_check_feature(dev, DRIVER_SYNCOBJ)）。
 * smithay（niri 的底层）就是靠这两个 cap 决定要不要走显式同步：
 * 声明了它才会把渲染完成的 fence 通过 IN_FENCE_FD 传下来，我们
 * prepare_fb 里等到的就是真 GPU fence；不声明它就退化成"隐式同步"，
 * 而高通 kgsl 不做隐式同步 —— 整条链零同步，屏幕上会扫到没画完的帧
 * （"每新来一个窗口闪一下"就是这么来的）。
 *
 * syncobj 本身是 DRM 核心实现（drm_syncobj.c），与具体驱动无关，
 * 虚拟卡声明它是正当的。*/
#define VDRM_DRIVER_FEATURES     (DRIVER_GEM | DRIVER_MODESET | DRIVER_ATOMIC | \
                                  DRIVER_SYNCOBJ | DRIVER_SYNCOBJ_TIMELINE)

/* 是否额外暴露一个 DRM render 节点（renderDxxx）。
 *
 * 虚拟卡没有 GPU：这个节点不提供任何执行力，只让 (card, render) 在合成器
 * 和 libdrm 眼里成对 —— drmGetDevices2() 能看到两个节点、drmDevicesEqual()
 * 成立，KWin/wlroots 这类按"primary + render 成对"配对的合成器就不用再手工
 * 配两个路径。真正的渲染由用户态 mesa 打开真 GPU（本机是 KGSL）完成，
 * render 节点只用于 PRIME 之类的 core ioctl。
 *
 * 永远暴露（不再做 render_node A/B 开关）：这是标准做法 —— 只有 primary
 * 节点、没有 render 节点的设备会让一部分合成器/库直接放弃硬件路径。*/

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

/* vdrm_audio.c */
extern int vdrm_audio_init(struct device *parent);
extern void vdrm_audio_exit(void);
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

/* 只支持线性布局。显式报出去（而不是不报）有两个好处：
 *   1. 内核会给 plane 建 IN_FORMATS blob、并置 DRM_CAP_ADDFB2_MODIFIERS，
 *      合成器就不会再用"先带 modifier 失败、再退回 invalid modifier"那条路
 *      —— niri 日志里那句 `will try with invalid modifier` 就是退路；
 *   2. 合成器知道我们只能线性，就不会把 GPU 的 tiled buffer 直接丢过来。
 * 内核这边 drm_simple_kms_format_mod_supported() 本身就只认 LINEAR，
 * dumb buffer 也是线性，所以报这个和实现是一致的，不是谎报。*/
/* ---- 运行时配置：一律"先准备，再 commit"两段式 ----
 *
 * mode / refresh / edid_dpi 三个参数都会改变合成器看到的输出描述（mode 改
 * 尺寸，refresh 进 detailed timing + vrate range，dpi 改 EDID 里的 mm），
 * 所以三者走同一套时序：
 *
 *   写参数  = 只"准备"：记下值、置 pending，不惊动合成器。
 *   写 commit = 一次性生效：重算 EDID + hotplug + 预热 vblank。
 *
 * 为什么不能就地生效：合成器什么时候重读 EDID 是不可控的（有的跟 hotplug，
 * 有的定时轮询）。就地改会出现"参数改了但屏幕没变"或者"尺寸新、刷新率旧"
 * 的半生效状态。commit 是唯一确定的时间点，也是池/转屏能对齐的那个点。
 *
 * 只有 mode 变了才需要重建池：refresh / dpi 不改几何，池里的槽照用。*/
#define VDRM_CFG_MODE    (1u << 0)
#define VDRM_CFG_REFRESH (1u << 1)
#define VDRM_CFG_DPI     (1u << 2)
static u32 g_cfg_pending;   /* 已准备、等 commit 的配置项（VDRM_CFG_*）*/

/* EDID 里报的物理尺寸决定合成器算出来的 DPI，进而决定它自己的 scale。
 * 0 = 用默认（16 cm 宽，1920 宽时约 305 dpi）；>0 时按这个 DPI 反推 mm。
 * 注意这不是"真实物理尺寸"，而是**给合成器的缩放建议**：
 *   dpi 越大 → 合成器 scale 越大 → 逻辑分辨率越小、UI 越大。*/
static int g_edid_dpi;

static int vdrm_edid_dpi_get(char *buf, const struct kernel_param *kp)
{
	(void)kp;
	return scnprintf(buf, PAGE_SIZE, "%d\n", g_edid_dpi);
}

static int vdrm_edid_dpi_set(const char *val, const struct kernel_param *kp)
{
	int v;

	(void)kp;
	if (kstrtoint(val, 0, &v))
		return -EINVAL;
	/* 上限给足但要挡住明显的笔误（比如把像素当 dpi 填进来）：
	 * mm = px / dpi * 25.4，超过 1000 时 mm 会小于 1，某些合成器会把
	 * 这种 EDID 当坏的丢掉。*/
	if (v < 0 || v > 1000) {
		pr_err("vdrm: edid_dpi %d out of range (0..1000)\n", v);
		return -ERANGE;
	}
	/* 加载时（g_dev 还没建）只是记值：没有合成器可通知。*/
	if (g_dev && v != g_edid_dpi)
		g_cfg_pending |= VDRM_CFG_DPI;
	g_edid_dpi = v;
	return 0;
}

static const struct kernel_param_ops vdrm_edid_dpi_ops = {
	.set = vdrm_edid_dpi_set,
	.get = vdrm_edid_dpi_get,
};
module_param_cb(edid_dpi, &vdrm_edid_dpi_ops, NULL, 0644);
MODULE_PARM_DESC(edid_dpi,
		 "DPI to advertise in EDID (0 = default 160x90mm); write then commit to apply");

static const u64 g_formats_mod[] = {
	DRM_FORMAT_MOD_LINEAR,
	/* 必须有这个哨兵：drm_universal_plane_init() 是"走到 INVALID 为止"
	 * 来数 modifier 的（不像 formats 那样另有 count 参数）。
	 * 少了它内核会一路读出数组边界 —— 实测在 insmod 里直接
	 * Oops（__drm_universal_plane_init+0x520，读 rodata 之后的未映射页），
	 * 整个手机重启。*/
	DRM_FORMAT_MOD_INVALID,
};

/* ---- M4: vblank 模拟（虚拟 device 没有真实扫描，用 hrtimer 造 vblank）---- */
static struct hrtimer g_vblank_timer;
static int            g_vblank_timer_inited;
static volatile int   g_vblank_on;
static volatile int   g_vblank_timer_running;   /* 连续 vblank 网格是否在跑 */
static volatile u64   g_vblank_period_ns = 16666666ULL; /* 60Hz 兜底（pipe enable 时会按 mode 重算）*/
/* 下一个 vblank 的绝对时刻。停机期间不重置 —— 相位按真实时间走，重新开网格时
 * 能接回原来的节拍，而不是"从现在再等一个周期"。*/
static ktime_t        g_vblank_next;

/* 诊断：网格在不在跑 / 累计 tick。空闲时应该是 running=0、ticks 不再增长。
 * 以前网格是常开的，没人看 vblank 也每秒醒 120 次；这两个值用来验收"按需"确实生效。*/
static unsigned int   g_vblank_running_view;
static unsigned long long g_vblank_ticks;
/* 一次性 vblank 预热：>0 表示还剩几个 tick 就自动停网格。
 * 只在"切分辨率"这种需要合成器马上重画一帧的时刻点亮，平时是 0。*/
static unsigned int   g_vblank_kick_ticks;
module_param_named(vblank_running, g_vblank_running_view, uint, 0444);
MODULE_PARM_DESC(vblank_running, "1 while the virtual vblank grid is armed");
module_param_named(vblank_ticks, g_vblank_ticks, ullong, 0444);
MODULE_PARM_DESC(vblank_ticks, "Virtual vblank ticks delivered so far");

/* 诊断旋钮：为 1 时强制虚拟显示的帧率（Hz），0 = 按 mode 反推（默认行为）。
 * 用来分辨“合成器跟不上”是 vblank 定拍造成的，还是它自己渲染慢。*/
#define VDRM_VBLANK_HZ_FORCE 0

/* ============= App <-> 模块 的契约（ioctl 结构体，两边必须逐字节一致）============= */

/* ABI 2：结构布局没变（vdrm_info 还是 24 字节），变化的是 flags 的语义 ——
 * 从"保留/全 0"变成能力位图。老 App 不看这个字段，老模块（flags==0）会被
 * 新 App 当成"没有能力位图"，按 ABI 1 的行为处理。所以这次加位是向前向后
 * 都安全的，不需要新 ioctl 号。*/
#define VDRM_ABI_VERSION 2
#define VDRM_POOL_MAX    32
/* 【ABI 常量，别跟着 VDRM_POOL_MAX 改】：GET_POOL 的 ioctl 号里编码了这个
 * 结构的大小，一变就对不上，App 会直接拿到 ENOTTY（实测：把池上限从 16 改到
 * 32 时，App 一块 buffer 都 forge 不出来，屏幕全黑）。内部池上限可以继续涨，
 * 只要不超过这个数。*/
#define VDRM_POOL_ABI_MAX 32
#if VDRM_POOL_MAX > VDRM_POOL_ABI_MAX
#error "VDRM_POOL_MAX 不能超过 ABI 上限 VDRM_POOL_ABI_MAX"
#endif
#define VDRM_MAX_INTS    40

/* drm_mode 的 type/flags 位（取值自 include/uapi/drm/drm_mode.h）。
 * 这几个位别用字面量写：PREFERRED 是 bit3、DRIVER 是 bit6，抄反了不会报错，
 * 只会让合成器挑错模式（见 get_modes 里的注释）。*/
#define VDRM_MODE_TYPE_PREFERRED (1 << 3)
#define VDRM_MODE_TYPE_DRIVER    (1 << 6)
#define VDRM_MODE_FLAG_PHSYNC    (1 << 0)
#define VDRM_MODE_FLAG_PVSYNC    (1 << 2)

struct vdrm_info {
	u32 abi_version;
	u32 width, height, format, pitch;
	u32 pool_capacity;
	u32 flags;     /* 能力位图，见 VDRM_CAP_*（0 = 老模块，没有能力位图）*/
	u32 reserved;
};

/* GET_INFO.flags 的位。用户态别拿 abi_version 比大小，要问"你支持这个吗"：
 * 老模块 flags==0 什么都不支持，新模块自己报。这样 App 可以比模块新，也可以
 * 比模块旧，不会互相掐死。
 *
 * 【只增不改】：位分配出去就不复用；删能力靠不再置位（而不是改语义）。*/
#define VDRM_CAP_MODE_RUNTIME     (1u << 0) /* mode 运行时可写，配 commit 生效 */
#define VDRM_CAP_REFRESH_RUNTIME  (1u << 1) /* refresh 同上 */
#define VDRM_CAP_DPI_RUNTIME      (1u << 2) /* edid_dpi 同上 */
#define VDRM_CAP_EVENTFD          (1u << 3) /* SET_EVENTFD：commit/pipe_enable 推事件 */
#define VDRM_CAP_PRESENTED        (1u << 4) /* PRESENTED：App 报告真实呈现时刻 */
#define VDRM_CAP_POOL             (1u << 5) /* GET_POOL + REGISTER：池中继 */
#define VDRM_CAP_INPUT_BATCH      (1u << 6) /* INPUT_BATCH：批量键/位移 */
#define VDRM_CAP_TOUCH            (1u << 7) /* TOUCH：多点绝对坐标 */
#define VDRM_CAP_POOL_HANDLE_GONE (1u << 8) /* 槽能在 GEM handle 先死后等 RETURN */
#define VDRM_CAP_READY            (1u << 9) /* READY：回前台重推当前帧 */
#define VDRM_CAPS_ALL (VDRM_CAP_MODE_RUNTIME | VDRM_CAP_REFRESH_RUNTIME | \
		       VDRM_CAP_DPI_RUNTIME | VDRM_CAP_EVENTFD | \
		       VDRM_CAP_PRESENTED | VDRM_CAP_POOL | \
		       VDRM_CAP_INPUT_BATCH | VDRM_CAP_TOUCH | \
		       VDRM_CAP_POOL_HANDLE_GONE | VDRM_CAP_READY)
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
/* 去掉 size 那 14 位，只留 dir/type/nr —— 用来做"结构大小无关"的匹配。*/
#define VDRM_IOC_BASE(cmd) \
	((unsigned int)(cmd) & ~(unsigned int)(0x3fffu << 16))
#define VDRM_IOC_SIZE(cmd) (((unsigned int)(cmd) >> 16) & 0x3fffu)
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
/* App 上报自己窗口的真实尺寸。模块用它推出"全面屏"的横竖两档分辨率，
 * 这样机型分辨率、override、以后分屏都不需要写死。
 * 必须在合成器第一次探测 mode 之前调用（App 在 nativeStart 里做）。*/
struct vdrm_screen_req {
	u32 width;
	u32 height;
};
#define VDRM_IOCTL_SET_SCREEN  VDRM_IOC(1, 'V', 0x0b, sizeof(struct vdrm_screen_req))
/* App 的事件通道：commit / g_active 变化时 signal。watcher 已经退役，
 * 这是唯一的用户态事件 fd。*/
#define VDRM_IOCTL_SET_EVENTFD VDRM_IOC(1, 'V', 0x04, sizeof(s32))

/* 读"配置"（mode 参数），不是"合成器眼下在用什么"。
 *
 * App 拿它决定自己窗口的横竖：配置是 1080x1920（竖）就让 Activity 转成竖屏，
 * 配置是 1920x1080 就转成横屏。这样 App 的图形框架、模块广告出去的 mode 列表、
 * 合成器最后选中的 mode 三者方向一致，画面才能铺满而不是缩成一条。
 *
 * 和 GET_INFO 的区别：GET_INFO 的 width/height 是 g_active（合成器已经切换过去
 * 的尺寸），改配置和真正切换之间有延迟；方向这种事必须看配置，否则会晚一步。*/
struct vdrm_conf {
	u32 width;      /* 配置的输出宽（mode=…），0 = 没配过 */
	u32 height;
	u32 screen_w;   /* App 上报的窗口宽，0 = 还没上报 */
	u32 screen_h;
	u32 mode_gen;   /* 每次运行时改 mode（prepare）都 +1 */
	u32 commit_gen; /* 每次 commit 都 +1：App 靠它提前转窗口 */
};
#define VDRM_IOCTL_GET_CONF    VDRM_IOC(2, 'V', 0x0d, sizeof(struct vdrm_conf))

/* App 报告"这一帧已经真的呈现了"（SurfaceFlinger 的 present fence signal 之后）。
 *
 * 为什么需要它：合成器（niri）只认 DRM flip-done。我们的虚拟显示器没有真实
 * 扫描，flip-done 以前是定时器拍的 —— 于是"可以复用这块显存"的时刻和"屏幕上
 * 真的扫描完这一帧"脱钩，合成器会在 DPU 还在扫的时候覆写，表现为局部蓝块。
 * 让 App 把真实呈现时刻报回来，flip-done 就能挂在真实节拍上。
 *
 * 完成条件：present（就在这个 ioctl 里就地完成）；present 一直不来则由
 * VDRM_FLIP_FALLBACK_MS 的存活超时兜底。*/
struct vdrm_present_req { u32 id; u32 flags; };
#define VDRM_IOCTL_PRESENTED \
	VDRM_IOC(1, 'V', 0x0e, sizeof(struct vdrm_present_req))

/* App 回到前台：把当前帧重推一次。arg 不用（写什么都行）。
 * 见下面 VDRM_IOCTL_READY 的处理。*/
#define VDRM_IOCTL_READY \
	VDRM_IOC(1, 'V', 0x0f, sizeof(s32))

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
	struct vdrm_pool_info items[VDRM_POOL_ABI_MAX];
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
	/* 这个槽最近一次上屏提交的时刻。用于 present_lat_us 诊断，
	 * 也是 safe_present 模式的兜底超时起点。*/
	u64 pend_since_ns;
	/* 【只在 safe_present=1 时非空】挂在这个槽上、等 App 报 present 才完成的
	 * flip-done。默认模式（safe_present=0）下事件直接 arm 到下一个 vblank，
	 * 这个字段一直是 NULL —— 见 vdrm_pipe_update 里的结论。*/
	struct drm_pending_vblank_event *pend_event;
	/* 分配占用：从 CREATE_DUMB 成功到合成器销毁那个 dumb/GEM handle 为止。
	 * 它保护的是“同一次分配”的别名：只要合成器还持有 handle，就绝不把
	 * 同一片内存再发给另一个 BO。释放点有三个：
	 *   1) dumb_destroy / GEM_CLOSE（正常路径，见 vdrm_dumb_destroy）；
	 *   2) postclose（合成器整个退掉）；
	 *   3) dumb_create 里对 handle_count == 0 的懒回收（兜底）。
	 * 注意：DISPL 槽即使 handle 已经销毁，也要等 App RETURN 才放开，
	 * 因为那块显存可能还在扫描输出。*/
	u32 allocation_reserved;
	u32 handle_gone;     /* 合成器已销毁 handle，但 DISPL 还要等 App RETURN */
	struct drm_file *allocation_owner; /* 只用于归属判断，不解引用 */
	/* B/A′：把 App/helper 交来的【原始 file】持住，GET_POOL 时原样转发
	 * （绕开 drm_gem_prime_handle_to_fd——它会把身份变成 shmem 味）*/
	struct file *f0;   /* fd0 的 file（fget 持有）*/
	struct file *f1;   /* fd1 的 file */
	u32 num_ints;
	u32 ints[VDRM_MAX_INTS];
};

static struct vdrm_pool_entry g_pool[VDRM_POOL_MAX];

static int                 g_pool_next_id = 1;
/* 池槽的所有字段（含 used/f0/f1/obj）都只由这一把锁保护：提交路径（不能
 * 睡眠）和 ioctl 路径共用。需要睡眠的操作（dma_buf import、fget、fd 安装）
 * 一律在锁外做——先在锁内取引用，再放锁去用。*/
static DEFINE_SPINLOCK(g_pool_state_lock);
static struct miscdevice   g_misc;
static struct file_operations g_misc_fops;
static int                 g_misc_registered;


/* /dev/vdrm 的打开计数：只有最后一个使用者关闭时，才补发输入释放、
 * 清帧队列、把 DISPL 槽还给池。mkpool 的短暂 open/close 不会打断 App。*/
static atomic_t            g_misc_opens = ATOMIC_INIT(0);

/* 输出几何（唯一 connector 的 preferred mode，XRGB8888）。
 *
 * 默认 1920x1080；加载时 `insmod vdrm.ko mode=2376x1080` 覆盖。
 * 而且现在**运行时可写**：
 *     echo 2376x1080 > /sys/module/vdrm/parameters/mode
 * 写进去会：更新几何 → 按新尺寸重算 EDID → 发 hotplug 让合成器重新探测
 * 模式列表 → 唤醒正等着新尺寸槽的 dumb_create。池那边由 App 看到
 * GET_INFO 的宽高变化后重建（mode_gen 每次加一）。*/
/* 等"新尺寸的池槽"用的唤醒队列：运行时切分辨率时，合成器往往比 App 重建
 * 池更早来要新尺寸的 buffer，dumb_create 在这里等一会儿而不是直接 ENOSPC。*/
static DECLARE_WAIT_QUEUE_HEAD(g_pool_wq);

/* App 的 mode 事件：commit 时抬 commit_gen 后 signal 一次（Java 提前转窗口），
 * pipe_enable 时 g_active 变化再 signal 一次（Native 立刻重建池/几何）。*/
static struct eventfd_ctx *g_app_ctx;
static DEFINE_SPINLOCK(g_app_ctx_lock);

static void vdrm_app_signal(void)
{
	unsigned long flags;

	spin_lock_irqsave(&g_app_ctx_lock, flags);
	if (g_app_ctx)
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 9, 0)
		eventfd_signal(g_app_ctx);      /* 6.9 起去掉了 count 参数 */
#else
		eventfd_signal(g_app_ctx, 1);
#endif
	spin_unlock_irqrestore(&g_app_ctx_lock, flags);
}

static u32 g_mode_w = 1920, g_mode_h = 1080, g_mode_pitch = 1920 * 4;
static char g_mode_str[16] = "1920x1080";
/* 每次运行时改 mode 都 +1。用户态把它当"池该按新尺寸重建了"的信号。*/
static u32 g_mode_gen;
/* 每次真正 commit（EDID + hotplug）都 +1。App 的 Java 侧靠它把"申请转屏"
 * 提前到 commit 这一拍，让 Android 的窗口旋转动画和合成器换档重叠；
 * 用它而不是 mode_gen：prepare 之后 mkpool 还要建槽，可能隔几百毫秒。*/
static u32 g_commit_gen;
/* 对外报的分辨率集合：基础两档（1080x1920 / 1920x1080）+ App 上报的
 * 屏幕尺寸的横竖两档 + **当前 mode 参数那一档**。合成器可以在这几个里自由切
 * （一次 atomic modeset），不需要热插拔、不需要重启合成器。
 *
 * 最后那一档是要紧的：`echo WxH > mode` 只是改参数 + 建池，合成器换档靠的是
 * "热插拔后列表里有一个 PREFERRED 且可用"的模式。以前这个列表只装基础两档
 * 和 App 上报的尺寸，于是任何别处的分辨率（比如 1080x2376）都不会被报出去，
 * 热插拔后列表里一个 PREFERRED 都没有，合成器原地不动 —— 参数变了、屏没变，
 * 看起来就是"mode 指令失效"。现在把当前档并进列表，任意合法 WxH 都能切。
 *
 * 池最多 32 槽、每档 6 个 —— 六档刚好，不超。*/
#define VDRM_MAX_MODES 6
static u32 g_screen_w, g_screen_h;		/* App 上报的窗口尺寸（0 = 未知）*/
static u32 g_modes[VDRM_MAX_MODES][2];
static int g_mode_count;

static void vdrm_modes_rebuild(void)
{
	static const u32 base[][2] = {
		{ 1080, 1920 },
		{ 1920, 1080 },
	};
	int i, n = 0;

	for (i = 0; i < (int)ARRAY_SIZE(base) && n < VDRM_MAX_MODES; i++) {
		g_modes[n][0] = base[i][0];
		g_modes[n][1] = base[i][1];
		n++;
	}
	if (g_screen_w && g_screen_h) {
		u32 cand[2][2] = {
			{ g_screen_w, g_screen_h },
			{ g_screen_h, g_screen_w },
		};

		for (i = 0; i < 2 && n < VDRM_MAX_MODES; i++) {
			int j, dup = 0;

			for (j = 0; j < n; j++)
				if (g_modes[j][0] == cand[i][0] &&
				    g_modes[j][1] == cand[i][1])
					dup = 1;
			if (!dup) {
				g_modes[n][0] = cand[i][0];
				g_modes[n][1] = cand[i][1];
				n++;
			}
		}
	}
	/* 当前 mode 参数这一档必须进列表：它是热插拔后唯一的 PREFERRED
	 * （见 vdrm_conn_get_modes 里那段的注释），合成器就是靠它决定换到哪一档。*/
	if (n < VDRM_MAX_MODES) {
		int j, dup = 0;

		for (j = 0; j < n; j++)
			if (g_modes[j][0] == g_mode_w && g_modes[j][1] == g_mode_h)
				dup = 1;
		if (!dup) {
			g_modes[n][0] = g_mode_w;
			g_modes[n][1] = g_mode_h;
			n++;
		}
	}
	g_mode_count = n;
}
/* 合成器"当前实际在用"的尺寸（pipe_enable / dumb_create 里更新）。
 * 池、GET_INFO、mode_slots 都跟着它走，而不是跟着 mode 参数。*/
static u32 g_active_w = 1920, g_active_h = 1080;

/* 只读：合成器**真正编进 pipe 的尺寸**（g_active）。
 *
 * 和 mode 参数的区别很要命：mode 是"我要求什么"，active 是"屏幕上真的是什么"。
 * 用户态（通知里的切换动作）拿它验证一次切换有没有真的生效 —— 合成器可能因为
 * 缓冲导入失败、热插拔没跟等原因原地不动，只看 mode 参数会以为成功了。*/
static int vdrm_active_get(char *buf, const struct kernel_param *kp)
{
	(void)kp;
	return scnprintf(buf, PAGE_SIZE, "%ux%u\n", g_active_w, g_active_h);
}
static const struct kernel_param_ops vdrm_active_ops = { .get = vdrm_active_get };
module_param_cb(active, &vdrm_active_ops, NULL, 0444);
MODULE_PARM_DESC(active, "Size the compositor actually programmed (g_active)");

static bool vdrm_res_advertised(u32 w, u32 h)
{
	int i;

	for (i = 0; i < g_mode_count; i++)
		if (g_modes[i][0] == w && g_modes[i][1] == h)
			return true;
	return w == g_mode_w && h == g_mode_h;
}

/* 只读：池里"尺寸等于当前生效尺寸"的槽有几个。mkpool 和诊断脚本靠它判断
 * 池是不是还缺当前分辨率的缓冲。*/
/* 只读：当前广告出去的分辨率列表，形如 "1080x1920,1920x1080,1264x2780,2780x1264"。
 * mkpool 直接读它，不需要自己维护一份。*/
static int vdrm_modes_get(char *buf, const struct kernel_param *kp)
{
	int i, off = 0;

	(void)kp;
	for (i = 0; i < g_mode_count && off < PAGE_SIZE - 24; i++)
		off += scnprintf(buf + off, PAGE_SIZE - off, "%s%ux%u",
				 i ? "," : "", g_modes[i][0], g_modes[i][1]);
	if (off < PAGE_SIZE - 1)
		buf[off++] = '\n';
	if (off < PAGE_SIZE)
		buf[off] = 0;
	return off;
}

static const struct kernel_param_ops vdrm_modes_ops = { .get = vdrm_modes_get };
module_param_cb(modes, &vdrm_modes_ops, NULL, 0444);
MODULE_PARM_DESC(modes, "Advertised resolutions, e.g. 1080x1920,1920x1080,...");

static int vdrm_mode_slots_get(char *buf, const struct kernel_param *kp)
{
	unsigned long flags;
	int i, n = 0;

	(void)kp;
	/* 按【目标尺寸】（mode 参数）数，不是按 g_active 数。
	 *
	 * mkpool 靠这个值判断"这一档还缺几个槽"。以前按 g_active 数，切换尺寸
	 * 时 g_active 还是旧尺寸，于是数出来的是旧档的槽数 —— mkpool 以为池够，
	 * 一个槽都不建，合成器真去要新尺寸的 buffer 就 ENOSPC
	 * （niri 日志：Failed to create dumb buffer / No space left on device）。
	 * 平时两者相等，只有"参数已改、合成器还没切"这段窗口里不同。*/
	spin_lock_irqsave(&g_pool_state_lock, flags);
	for (i = 0; i < VDRM_POOL_MAX; i++) {
		if (g_pool[i].used && g_pool[i].width == g_mode_w &&
		    g_pool[i].height == g_mode_h)
			n++;
	}
	spin_unlock_irqrestore(&g_pool_state_lock, flags);
	return scnprintf(buf, PAGE_SIZE, "%d\n", n);
}

static const struct kernel_param_ops vdrm_mode_slots_ops = {
	.get = vdrm_mode_slots_get,
};
module_param_cb(mode_slots, &vdrm_mode_slots_ops, NULL, 0444);
MODULE_PARM_DESC(mode_slots, "Slots in the pool that match the target (mode param) size");
module_param_named(mode_gen, g_mode_gen, uint, 0444);
MODULE_PARM_DESC(mode_gen, "Incremented every time the mode is changed at runtime");

static int vdrm_mode_apply(u32 w, u32 h, bool runtime);
static void vdrm_vblank_grid_start(void);
static void vdrm_vblank_grid_stop(void);
static void vdrm_conn_set_edid(struct drm_connector *conn);
static void vdrm_frame_clear(void);

/* 虚拟输出的刷新率（Hz）。vblank 周期由合成器编进 pipe 的 mode 反推
 * （clock/htotal/vtotal），所以改这里就够了：60 时 niri 出 60fps，120 时出
 * 120fps（宿主面板是 120 就对齐到 120）。默认 120。
 *
 * 它也进 EDID（detailed timing 的 vtotal + 0xFD 的 vrate range），所以要改
 * 必须走 commit —— 见文件上部 VDRM_CFG_* 那段。 */
static uint g_refresh = 120;

static int vdrm_refresh_get(char *buf, const struct kernel_param *kp)
{
	(void)kp;
	return scnprintf(buf, PAGE_SIZE, "%u\n", g_refresh);
}

static int vdrm_refresh_set(const char *val, const struct kernel_param *kp)
{
	unsigned int v;
	int rc;

	(void)kp;
	rc = kstrtouint(val, 0, &v);
	if (rc)
		return rc;
	/* 下限 24：再低连 UI 动画都连不上；上限 240：合成器侧没有意义，
	 * 而且 vblank 网格会空转。*/
	if (v < 24 || v > 240) {
		pr_err("vdrm: refresh %u out of range (24..240)\n", v);
		return -ERANGE;
	}
	if (g_dev && v != g_refresh)
		g_cfg_pending |= VDRM_CFG_REFRESH;
	g_refresh = v;
	return 0;
}

static const struct kernel_param_ops vdrm_refresh_ops = {
	.set = vdrm_refresh_set,
	.get = vdrm_refresh_get,
};
module_param_cb(refresh, &vdrm_refresh_ops, NULL, 0644);
MODULE_PARM_DESC(refresh,
		 "Virtual output refresh rate in Hz (24..240, default 120); write then commit to apply");

static int vdrm_mode_apply(u32 w, u32 h, bool runtime)
{
	/* 范围与 mode_config 的 max 一致（4096），下限取 16 —— 再小 CVT 的
	 * porch 都放不下。*/
	if (w < 16 || h < 16 || w > 4096 || h > 4096) {
		pr_err("vdrm: mode %ux%u out of range (16..4096)\n", w, h);
		return -ERANGE;
	}

	g_mode_w     = w;
	g_mode_h     = h;
	g_mode_pitch = ALIGN(w * 4, 64);
	scnprintf(g_mode_str, sizeof(g_mode_str), "%ux%u", w, h);

	/* 目标档必须进"对外广告"列表 —— 热插拔后合成器就是靠列表里那个
	 * PREFERRED 决定换到哪一档。不重建的话，非基础档（比如 1080x2376）
	 * 改了参数、建了池、也 commit 了，但列表里没有它，合成器原地不动：
	 * 参数变了、屏没变，就是"mode 指令失效"。*/
	vdrm_modes_rebuild();

	/* 这里【不要】更新 g_active_w/h。
	 *
	 * g_active 的语义是"合成器此刻真正在用的尺寸"，只有 pipe_enable /
	 * dumb_create 才能改它 —— App 靠 GET_INFO 的这两个字段决定要不要转屏。
	 * 之前图省事在这儿同步过一版，结果运行中改 mode 参数时 App 先转屏、
	 * 合成器还停在旧尺寸，屏幕上就是"App 竖着、画面横着"。
	 *
	 * 池要按"目标尺寸"建这件事由 mkpool 负责：它直接读 mode 参数取目标尺寸，
	 * 不依赖 g_active。*/

	if (!runtime)
		return 0;

	/* 运行中改 mode = 只是"准备"。
	 *
	 * 这里【不】改 EDID、【不】发 hotplug：合成器还按旧档跑着，等用户态把新
	 * 尺寸的槽建好（mkpool），再写 commit 参数才真正广播出去。否则合成器
	 * 收到热插拔会立刻换档，而池里还没有新尺寸的槽 —— 画面卡在上一帧，
	 * 看起来就是"合成器切了、池没跟上"。
	 *
	 * 这个时序与合成器无关：跟 hotplug 的（niri/KWin/Hyprland）在 commit
	 * 之后自己换档；不跟的用它自己的工具切，那时候槽也已经在了。*/
	g_cfg_pending |= VDRM_CFG_MODE;
	g_mode_gen++;
	wake_up_interruptible(&g_pool_wq);
	pr_debug("vdrm: mode prepared -> %ux%u pitch=%u (commit pending)\n",
		w, h, g_mode_pitch);
	return 0;
}

/* mode 参数：加载时可写（insmod），运行时可写（echo 到 sysfs）。*/
static int vdrm_mode_param_set(const char *val, const struct kernel_param *kp)
{
	unsigned int w, h;
	char tail;
	int rc;

	(void)kp;
	/* 严格 WxH：多一个字符就拒绝（1920x1080@60 这种不算）。
	 * 注意 `echo 2376x1080 > .../mode` 会带一个换行符 —— 那是允许的。*/
	{
		int n = sscanf(val, "%ux%u%c", &w, &h, &tail);

		if (!(n == 2 || (n == 3 && (tail == '\n' || tail == '\r')))) {
			pr_err("vdrm: bad mode '%s' (expect WxH)\n", val);
			return -EINVAL;
		}
	}
	/* 加载时（g_dev 还没建）只更新几何；运行时的写入才发 hotplug / 抬
	 * mode_gen / 唤醒等槽的人。*/
	rc = vdrm_mode_apply(w, h, g_dev != NULL);
	return rc;
}

static int vdrm_mode_param_get(char *buf, const struct kernel_param *kp)
{
	(void)kp;
	return scnprintf(buf, PAGE_SIZE, "%s\n", g_mode_str);
}

static const struct kernel_param_ops vdrm_mode_param_ops = {
	.set = vdrm_mode_param_set,
	.get = vdrm_mode_param_get,
};
module_param_cb(mode, &vdrm_mode_param_ops, NULL, 0644);

/* commit：把"准备好的"配置真正广播出去 —— 重算 EDID + 发 hotplug。
 *
 * mode / refresh / edid_dpi 三个参数共用这一个时间点（见文件上部 VDRM_CFG_*）。
 * 写什么都行（echo 1 > .../commit），值不参与；没有 pending 时是空操作，
 * 所以重复写、开机时写都安全。mkpool --mode WxH 会在建完池之后自动写一次。*/
static int vdrm_commit_set(const char *val, const struct kernel_param *kp)
{
	u32 pending;

	(void)val;
	(void)kp;
	if (!g_cfg_pending)
		return 0;

	pending = g_cfg_pending;
	g_cfg_pending = 0;
	g_commit_gen++;
	/* 先叫醒 App：Java 侧靠 commit_gen 在 hotplug 的同时申请转屏，
	 * 窗口旋转动画才能和合成器换档重叠。*/
	vdrm_app_signal();
	/* 切档瞬间给 vblank 预热几个 tick：合成器一收到 hotplug 就会重新提交，
	 * 这几拍能让挂起的 flip-done 及时完成、它马上再出一帧 —— 图层就能及时
	 * 换成新尺寸的画面，而不用靠"先黑一下/先贴旧帧"兜。平时网格仍是停的。*/
	g_vblank_kick_ticks = 3;
	vdrm_vblank_grid_start();
	/* 队列里上一档的帧事件只有【尺寸变了】才必须丢：换档后 App 会按新
	 * 几何贴图，旧尺寸的尾帧贴上去就是错位/回滚。只改 refresh 或 dpi 时
	 * 几何不变，那条尾帧是合法的 —— 丢掉反而让合成器等一个永远不来的
	 * flip-done（表现为换刷新率后卡一下）。*/
	if (pending & VDRM_CFG_MODE)
		vdrm_frame_clear();
	if (g_conn && g_dev)
		vdrm_conn_set_edid(g_conn);
	if (g_dev && g_registered)
		drm_kms_helper_hotplug_event(g_dev);
	pr_info("vdrm: commit -> %ux%u@%u dpi=%d (pending=0x%x mode_gen=%u commit_gen=%u)\n",
		g_mode_w, g_mode_h, g_refresh, g_edid_dpi, pending,
		g_mode_gen, g_commit_gen);
	return 0;
}
static const struct kernel_param_ops vdrm_commit_ops = {
	.set = vdrm_commit_set,
};
module_param_cb(commit, &vdrm_commit_ops, NULL, 0200);
MODULE_PARM_DESC(commit,
		 "apply prepared config (mode/refresh/edid_dpi): rebuild EDID + hotplug");
MODULE_PARM_DESC(mode,
		 "Virtual output mode WxH (writable at runtime, e.g. 2376x1080); write then commit");

static int vdrm_parse_mode_param(void)
{
	if (g_refresh < 24 || g_refresh > 240) {
		pr_err("vdrm: refresh %u out of range (24..240)\n", g_refresh);
		return -ERANGE;
	}
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
/* 诊断：pipe_update 匹配/未匹配次数（只读 param）*/
static unsigned long long g_frames_pushed;
static unsigned long long g_frames_unmatched;
/* 叠写计数：合成器又扫描输出了一块"App 还没归还"的槽。
 *
 * 这是零拷贝链路上撕裂的硬指标：槽还是 DISPL（App 拿到帧事件、还没 RETURN），
 * 合成器就又开始往同一片显存里画下一帧 —— 屏幕上正显示着这块，就会看到闪/撕。
 * 正常流水线下这个值应该一直是 0。*/
static unsigned long long g_frames_reuse;
module_param_named(frames_pushed, g_frames_pushed, ullong, 0444);
module_param_named(frames_unmatched, g_frames_unmatched, ullong, 0444);
module_param_named(frames_reuse, g_frames_reuse, ullong, 0444);

/* 注意：这里没有“stale DISPL 超时回收”。
 *
 * 旧实现会在 dumb_create 等不到槽时把最老的 DISPL 槽改成 FREE。它救不了
 * 分配 —— 那个槽的 allocation_reserved 还在，选槽阶段根本不会要它。
 *
 * 现在靠真正的生命周期回收：dumb_destroy/GEM_CLOSE 解除分配占用，RETURN
 * 解除显示占用。App 真丢了 release 回调时由 App 侧的 held_expire（5s）
 * 或 misc_release 兜底，不需要内核去猜“这块是不是还在屏上”。*/
static unsigned long long g_frame_flip_present_done;   /* present 驱动完成的计数 */
module_param_named(flip_present_done, g_frame_flip_present_done, ullong, 0444);

/* 诊断：从"合成器提交这一帧"到"App 报 present"的耗时。
 *
 * 这段时间 = App 拿到帧事件 → forge/latch 到自己的 SurfaceControl 图层 →
 * SF 合成 → 扫描输出 → present fence signal。合成器是"flip-done 才开始画
 * 下一帧"的串行循环，所以它基本上就是帧周期的下界；想知道"为什么只有
 * 30fps"，先看这两个数。*/
static unsigned long long g_present_lat_us, g_present_lat_max_us;
module_param_named(present_lat_us, g_present_lat_us, ullong, 0444);
MODULE_PARM_DESC(present_lat_us, "commit -> App reported present, last (us)");
module_param_named(present_lat_max_us, g_present_lat_max_us, ullong, 0444);
MODULE_PARM_DESC(present_lat_max_us, "commit -> App reported present, max (us)");


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

	/* drm_gem_mmap_obj() 为这次映射取了一份 GEM 引用，等的是 vm_close 归还；
	 * 但下面 dma_buf_mmap() 会把 vm_ops 换成 dma_buf 的、vma 绑到 dmabuf 的
	 * file 上 —— 那个 close 不认识 GEM，这份引用就永远没人还。结果是池对象
	 * 泄漏、mmap offset 节点留在 drm_mm 里，卸载时 drm_mm_takedown() WARN
	 * "Memory manager not clean during takedown"（实测每次 mmap 一次）。
	 * 映射本身由 dmabuf 的 file 引用续命，所以这里直接把那份引用还掉。*/
	vma->vm_private_data = NULL;
	vma->vm_ops = NULL;
	ret = dma_buf_mmap(obj->import_attach->dmabuf, vma, 0);
	if (!ret)
		drm_gem_object_put(obj);
	return ret;
}

/*
 * dumb_create：只从池里发缓冲（伪装成 card1 原生 dumb）；池由 mkpool 建好。
 * 没有匹配/空闲槽时直接 -ENOSPC —— 不保留第二套 heap 出身（那批 buffer
 * 没有原始 fd0/fd1，App 根本 forge 不出来，只会变成不可显示的隐藏状态）。
 */
/* 解除一个槽的“分配占用”。调用者必须持 g_pool_state_lock。
 * 只有确认 handle 已经销毁（或整个 drm_file 已经关掉）时才调用。*/
static bool vdrm_slot_release_allocation(int i)
{
	struct vdrm_pool_entry *e = &g_pool[i];

	if (!e->used || !e->allocation_reserved)
		return false;
	e->allocation_reserved = 0;
	e->allocation_owner    = NULL;
	e->handle_gone         = 0;
	if (e->state == VDRM_ST_TAKEN)
		e->state = VDRM_ST_FREE;
	return true;
}

/* 分配占用的兜底回收。合成器如果绕过 DESTROY_DUMB、直接用 GEM_CLOSE 把
 * handle 关掉，我们的 dumb_destroy 就收不到通知。这里在选槽前顺手把
 * “handle 已经没了、且不在屏上”的槽放开。
 *
 * DISPL 槽绝不能在这里动：它可能还在扫描输出，必须等 App RETURN。 */
static bool vdrm_slot_reap_closed(int i)
{
	struct vdrm_pool_entry *e = &g_pool[i];

	if (!e->used || !e->allocation_reserved || !e->obj)
		return false;
	if (e->state == VDRM_ST_DISPL)
		return false;
	if (READ_ONCE(e->obj->handle_count) != 0)
		return false;
	return vdrm_slot_release_allocation(i);
}

/*
 * 换档之后回收"别的尺寸、而且合成器已经放手"的整批槽。
 *
 * 为什么需要：池是**按档累积**的 —— 每试一个新分辨率就多 6 块，而
 * VDRM_POOL_MAX 是 32，也就是最多 5 档。试到第 6 档时 mkpool 就没地方建
 * 新槽了，失败现象却是 dumb_create -ENOSPC / 黑屏，根本看不出是"历史档
 * 占满了"。所以换档成功后把旧档整批扔掉，池始终只留当前档（+ 正在退场的
 * 那一档）。
 *
 * 安全条件（三条都要满足，宁可晚清不要错清）：
 *   1) 尺寸不是当前 mode 参数；
 *   2) 不在 DISPL —— 那块可能还在屏上，必须等 App RETURN；
 *   3) 合成器已经不再持有它（handle 已销毁，或 handle_count 归零）。
 *
 * 调用点都是进程上下文（dumb_create / pipe_enable），可以睡眠着放引用。*/
static void vdrm_pool_drop_stale(void)
{
	struct drm_gem_object *objs[VDRM_POOL_MAX];
	struct file *p0[VDRM_POOL_MAX], *p1[VDRM_POOL_MAX];
	unsigned long flags;
	int i, n = 0;

	spin_lock_irqsave(&g_pool_state_lock, flags);
	for (i = 0; i < VDRM_POOL_MAX; i++) {
		struct vdrm_pool_entry *e = &g_pool[i];

		if (!e->used)
			continue;
		if (e->width == g_mode_w && e->height == g_mode_h)
			continue;                  /* 当前档：还要用 */
		if (e->state == VDRM_ST_DISPL)
			continue;                  /* 可能还在屏上 */
		if (!e->handle_gone &&
		    !(e->obj && READ_ONCE(e->obj->handle_count) == 0))
			continue;                  /* 合成器还攥着，下次再说 */

		p0[n]   = e->f0;
		p1[n]   = e->f1;
		objs[n] = e->obj;
		memset(e, 0, sizeof(*e));
		n++;
	}
	spin_unlock_irqrestore(&g_pool_state_lock, flags);

	for (i = 0; i < n; i++) {
		if (p0[i])
			fput(p0[i]);
		if (p1[i])
			fput(p1[i]);
		if (objs[i])
			vdrm_gem_put(objs[i]);
	}
	if (n)
		pr_debug("vdrm: dropped %d stale slot(s) (keep %ux%u)\n",
			n, g_mode_w, g_mode_h);
}

static int vdrm_dumb_create(struct drm_file *file, struct drm_device *dev,
			    struct drm_mode_create_dumb *args)
{
	struct drm_gem_object *obj = NULL;
	unsigned long flags;
	u32 pitch = 0, id = 0, waited;
	int i, pick = -1, rc;

	(void)dev;
	if (args->bpp != 32)
		return -EINVAL;

	/* 先扔掉上一个档留下的整批槽（合成器已经放手的那些）。放在这里是因为
	 * 它跑在进程上下文、可以睡眠着放引用，而且每次换档合成器一定会先来
	 * 这里要新尺寸的 buffer —— 正好是清理的时机。*/
	vdrm_pool_drop_stale();

	/* 这里【不更新】g_active_w/h。
	 *
	 * 合成器在 modeset 之前就会先来 dumb_create（要新尺寸的 buffer），而
	 * g_active 是"已经真正换上去的尺寸"，App 靠它决定转不转屏。以前在这里
	 * 提前改 g_active，App 就会在合成器还没 modeset 时就转屏 —— 手机上表现为
	 * "App 已经横过来了，容器画面还是竖的（被转了 90°）"。
	 * 现在只让 pipe_enable 改 g_active。*/
	if ((g_active_w != args->width || g_active_h != args->height) &&
	    vdrm_res_advertised(args->width, args->height)) {
		wake_up_interruptible(&g_pool_wq);
	}

	/* 选槽与"预占"在 state 锁里做：drm_gem_handle_create() 会分配内存
	 * （可睡眠），不能放在自旋锁里；但也不能只拿一把睡眠锁就改 state ——
	 * 提交路径（pipe_update）只在 state 锁下读它。预占后别的调用看不到
	 * 这个槽，handle 建失败再回滚。*/
	/* 运行时切分辨率：合成器会先于 App 重建池来要新尺寸的 buffer，这里
	 * 等一会儿（最多 6 秒，每次唤醒/超时都重扫一遍），别直接 ENOSPC 让
	 * 合成器报 "failed to create dumb buffer"。请求的尺寸不是当前 mode
	 * 的话等也没用，直接失败。*/
	for (waited = 0; ; waited += 100) {
		bool reaped = false;

		spin_lock_irqsave(&g_pool_state_lock, flags);
		for (i = 0; i < VDRM_POOL_MAX; i++) {
			struct vdrm_pool_entry *e = &g_pool[i];

			/* handle 已销毁、但没人放开的槽，先兜底回收 */
			if (vdrm_slot_reap_closed(i))
				reaped = true;

			if (!e->used || e->allocation_reserved || e->handle_gone ||
			    e->state != VDRM_ST_FREE || !e->f0 || !e->obj)
				continue;
			if (e->width != args->width || e->height != args->height)
				continue;
			e->state               = VDRM_ST_TAKEN;
			e->allocation_reserved = 1;
			e->allocation_owner    = file;
			e->handle_gone         = 0;
			obj   = e->obj;
			pitch = e->pitch;
			id    = e->id;
			pick  = i;
			break;
		}
		spin_unlock_irqrestore(&g_pool_state_lock, flags);
		if (reaped)
			wake_up_interruptible(&g_pool_wq);
		if (pick >= 0)
			break;

		if (args->width != g_mode_w || args->height != g_mode_h)
			return -ENOSPC;
		if (waited >= 6000)
			return -ENOSPC;
		if (wait_event_interruptible_timeout(g_pool_wq, false,
						     msecs_to_jiffies(100)) < 0)
			return -ERESTARTSYS;
	}


	/* 槽在 used=1 期间由池持着 obj 引用，这里 obj 不会消失 */
	rc = drm_gem_handle_create(file, obj, &args->handle);
	if (rc) {
		spin_lock_irqsave(&g_pool_state_lock, flags);
		vdrm_slot_release_allocation(pick);
		spin_unlock_irqrestore(&g_pool_state_lock, flags);
		return rc;
	}

	args->pitch = pitch;
	args->size  = obj->size;
	pr_debug("vdrm: dumb_create -> POOL id=%u handle=%u %ux%u pitch=%u\n",
		 id, args->handle, args->width, args->height, pitch);
	return 0;
}

/* 合成器销毁 dumb 缓冲（gbm_bo_destroy / drmModeDestroyDumbBuffer 都会走到
 * 这里）。这是“分配占用”真正的释放点：只要它到了，这个槽就不再被那个
 * drm_file 持有，可以重新发给下一个 CREATE_DUMB —— 除非 App 还在屏上显示
 * 这块（DISPL），那种情况只记 handle_gone，等 App RETURN 再放开。*/
static int vdrm_dumb_destroy(struct drm_file *file, struct drm_device *dev,
			     u32 handle)
{
	struct drm_gem_object *obj;
	unsigned long flags;
	bool released = false;
	int i;

	(void)dev;

	obj = drm_gem_object_lookup(file, handle);
	if (!obj)
		return -ENOENT;

	spin_lock_irqsave(&g_pool_state_lock, flags);
	for (i = 0; i < VDRM_POOL_MAX; i++) {
		struct vdrm_pool_entry *e = &g_pool[i];

		if (!e->used || e->obj != obj)
			continue;
		/* 这个对象上可能还有别的 handle（同文件重复 handle / 别的
		 * drm_file PRIME 回来）。只有最后一个 handle 销毁时才算真正
		 * 放开分配权，否则仍可能有人拿同一片内存画。
		 * 这里按 obj 匹配而不是按 allocation_owner：如果原 owner 的
		 * handle 先销毁、最后一个 handle 由别的 drm_file 关掉，也要
		 * 能收到这次“最后一次”。*/
		if (READ_ONCE(e->obj->handle_count) > 1)
			break;
		if (e->state == VDRM_ST_DISPL) {
			/* 这块可能还在扫描输出：只记“handle 没了”，
			 * 分配权留给 RETURN 放，避免同一片内存被别名复用。*/
			e->handle_gone      = 1;
			e->allocation_owner = NULL;
		} else {
			released = vdrm_slot_release_allocation(i);
		}
		break;
	}
	spin_unlock_irqrestore(&g_pool_state_lock, flags);
	if (released)
		wake_up_interruptible(&g_pool_wq);

	drm_gem_object_put(obj);
	return drm_gem_handle_delete(file, handle);
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

/* 换档/关输出时丢弃“还没被 App 取走”的旧帧事件。
 * 不清 g_frame_seq：App 用它去重（seq == last_seq），重置反而可能让同一格
 * 序号被当成重复帧丢掉。*/
static void vdrm_frame_clear(void)
{
	unsigned long flags;

	flags = _raw_spin_lock_irqsave(&g_frame_lock.rlock);
	g_frame_has = false;
	_raw_spin_unlock_irqrestore(&g_frame_lock.rlock, flags);
}

/* App 刚接上事件通道时，把“当前正在扫描输出的那一帧”补推一次。
 *
 * 场景：App 重启时合成器是静态的（没有新提交），或者换档那一拍只有一张
 * 旧尺寸的尾帧（被 pipe_update 过滤掉）。这时 App 的池是空的、屏上也没有
 * 可贴的帧，看起来就是黑屏。把当前 plane 的 fb 对应的池槽补推一次，App
 * 就能立刻 latch 上屏，然后正常等下一帧。*/
static void vdrm_push_current_frame(void)
{
	struct drm_plane_state *ps;
	struct drm_framebuffer *fb;
	struct drm_gem_object *gobj;
	struct dma_buf *fbdb;
	unsigned long flags;
	u32 id = 0, ud = 0;
	int i;

	if (!g_pipe)
		return;
	ps = g_pipe->plane.state;
	if (!ps || !ps->fb)
		return;
	fb = ps->fb;
	gobj = fb->obj[0];
	fbdb = (gobj && gobj->import_attach) ? gobj->import_attach->dmabuf : NULL;

	spin_lock_irqsave(&g_pool_state_lock, flags);
	for (i = 0; i < VDRM_POOL_MAX; i++) {
		struct drm_gem_object *po;
		struct dma_buf *pdb;

		if (!g_pool[i].used)
			continue;
		po = g_pool[i].obj;
		pdb = (po && po->import_attach) ? po->import_attach->dmabuf : NULL;
		if (po == gobj || (fbdb && pdb && fbdb == pdb)) {
			g_pool[i].state = VDRM_ST_DISPL;
			id = g_pool[i].id;
			ud = g_pool[i].user_data;
			break;
		}
	}
	spin_unlock_irqrestore(&g_pool_state_lock, flags);
	if (id)
		vdrm_frame_push(id, ud);
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

/* ---- safe_present：保守模式（给陌生设备留的退路）----
 *
 * 0（默认）= flip-done 在下一个 vblank 完成，不等 SF 的 present。快：实测
 *            鼠标连续移动 88fps。代价是合成器可能在 SF 还没放手时就重提交
 *            同一块显存（`frames_reuse` 会涨），理论上会撕。
 * 1         = 老行为：事件挂到池槽上，等 App 报 present 才完成，500ms 兜底。
 *            慢（~30fps）但绝不覆写正在显示的显存。
 *
 * 换机器/换 GPU 之后如果看到"局部色块"，先把这个设成 1 验证是不是这条路径。*/
static bool g_safe_present;
module_param_named(safe_present, g_safe_present, bool, 0644);
MODULE_PARM_DESC(safe_present,
		 "1 = wait for App present before completing flip-done (30fps, never overwrites shown memory)");

/* 保守模式下最长等多久（毫秒）。present 不来（App 在后台/挂了）时兜底，
 * 不能让合成器永远等下去。*/
#define VDRM_SAFE_FALLBACK_MS 500

static unsigned long long g_flip_safe_present_done, g_flip_safe_fallback;
module_param_named(flip_safe_present_done, g_flip_safe_present_done, ullong, 0444);
module_param_named(flip_safe_fallback, g_flip_safe_fallback, ullong, 0444);

/* 完成一个挂在槽上的 flip-done。调用者不要持 g_pool_state_lock。*/
static void vdrm_flip_complete(struct drm_pending_vblank_event *ev)
{
	struct drm_crtc *crtc;

	if (!ev || !g_pipe || !g_pipe->crtc.dev)
		return;
	crtc = &g_pipe->crtc;
	_raw_spin_lock_irq(&crtc->dev->event_lock.rlock);
	drm_crtc_send_vblank_event(crtc, ev);
	_raw_spin_unlock_irq(&crtc->dev->event_lock.rlock);
}

static int vdrm_misc_release(struct inode *inode, struct file *file)
{
	bool last;
	unsigned long flags;
	int i;

	(void)inode;
	(void)file;

	last = atomic_dec_and_test(&g_misc_opens);
	pr_info("vdrm: misc release (last=%d, pool kept)\n", last);
	if (!last)
		return 0;

	/* App 没了：它注册的事件 fd 也要放掉，否则内核一直持有那个 eventfd */
	{
		struct eventfd_ctx *ctx;

		spin_lock_irqsave(&g_app_ctx_lock, flags);
		ctx = g_app_ctx;
		g_app_ctx = NULL;
		spin_unlock_irqrestore(&g_app_ctx_lock, flags);
		if (ctx)
			eventfd_ctx_put(ctx);
	}

	vdrm_input_reset_state();

	/* 清最新帧：否则新 App 会读到上一实例遗留的旧事件 */
	flags = _raw_spin_lock_irqsave(&g_frame_lock.rlock);
	g_frame_has = false;
	g_frame_seq = 0;
	_raw_spin_unlock_irqrestore(&g_frame_lock.rlock, flags);

	/* 没有消费者了：没人会再替 DISPL 槽 RETURN，全部放回 FREE。
	 * 池本身（obj/f0/f1）归模块所有，不在这里清。
	 * 挂在槽上等 RETURN 的 flip 事件也要一起完成，否则合成器会一直等。*/
	spin_lock_irqsave(&g_pool_state_lock, flags);
	{
		for (i = 0; i < VDRM_POOL_MAX; i++) {
			if (g_pool[i].used && g_pool[i].state == VDRM_ST_DISPL)
				g_pool[i].state = VDRM_ST_FREE;
		}
	}
	spin_unlock_irqrestore(&g_pool_state_lock, flags);

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

/* 把池表交给用户态。usersz 是【用户态那个结构】的字节数 —— 由 ioctl 号
 * 里的 size 字段带来。
 *
 * 为什么不能写死 sizeof(struct vdrm_pool_list)：这个大小参与 ioctl 编号，
 * 池上限一变编号就变，旧 App 直接吃 ENOTTY，一块 buffer 都 forge 不出来，
 * 屏幕全黑（实测过一次，见 CHANGES 第二十四项）。所以这里对 size 宽容：
 * 只按用户结构装得下的条目数回填，谁都能读，只是老 App 读到的槽数少。
 * 返回值不变，池里剩下的槽照样能被新 App 看到。*/
static long vdrm_pool_list_ioctl(void __user *arg, unsigned int usersz)
{
	struct file *snap0[VDRM_POOL_ABI_MAX] = { NULL };
	struct file *snap1[VDRM_POOL_ABI_MAX] = { NULL };
	struct vdrm_pool_list *list;
	unsigned long flags;
	unsigned int fit, copy_bytes;
	int i, n = 0;

	if (!g_dev)
		return -ENODEV;

	fit = (usersz > 8) ?
	      (usersz - 8) / (unsigned int)sizeof(struct vdrm_pool_info) : 0;
	if (fit > VDRM_POOL_ABI_MAX)
		fit = VDRM_POOL_ABI_MAX;
	copy_bytes = 8 + fit * (unsigned int)sizeof(struct vdrm_pool_info);
	if (copy_bytes > sizeof(struct vdrm_pool_list))
		copy_bytes = sizeof(struct vdrm_pool_list);

	/* 池表约 3.2KB：不放内核栈，也不用全局 —— 两个进程同时
	 * GET_POOL 会互相踩，按调用分配。*/
	list = kzalloc(sizeof(*list), GFP_KERNEL);
	if (!list)
		return -ENOMEM;

	/* 锁内只做快照 + 取自己的 file 引用（get_file 不睡眠）；
	 * fd 的安装可能因扩 fd 表而睡眠，放到锁外做。UNREGISTER 可以
	 * 在这期间把槽收走，但我们手上的引用保证 file/obj 仍然有效。*/
	spin_lock_irqsave(&g_pool_state_lock, flags);
	for (i = 0; i < VDRM_POOL_MAX && n < (int)fit; i++) {
		struct vdrm_pool_info *it = &list->items[n];
		u32 k;

		if (!g_pool[i].used || !g_pool[i].obj)
			continue;
		snap0[n] = g_pool[i].f0 ? get_file(g_pool[i].f0) : NULL;
		snap1[n] = g_pool[i].f1 ? get_file(g_pool[i].f1) : NULL;
		it->id       = g_pool[i].id;
		it->width    = g_pool[i].width;
		it->height   = g_pool[i].height;
		it->pitch    = g_pool[i].pitch;
		it->format   = g_pool[i].format;
		it->num_ints = g_pool[i].num_ints;
		for (k = 0; k < g_pool[i].num_ints && k < VDRM_MAX_INTS; k++)
			it->ints[k] = g_pool[i].ints[k];
		n++;
	}
	spin_unlock_irqrestore(&g_pool_state_lock, flags);

	list->count = (u32)n;
	for (i = 0; i < n; i++) {
		int fd = -1;

		if (snap0[i]) {
			fd = get_unused_fd_flags(0x80000 /*O_CLOEXEC*/);
			if (fd < 0)
				fput(snap0[i]);
			else
				fd_install((unsigned int)fd, snap0[i]);
		}
		list->items[i].fd = fd;

		fd = -1;
		if (snap1[i]) {
			fd = get_unused_fd_flags(0x80000 /*O_CLOEXEC*/);
			if (fd < 0)
				fput(snap1[i]);
			else
				fd_install((unsigned int)fd, snap1[i]);
		}
		list->items[i].fd1 = fd;
	}

	pr_debug("vdrm: GET_POOL -> %d entries (usersz=%u)\n", n, usersz);
	if (copy_to_user(arg, list, copy_bytes)) {
		kfree(list);
		return -EFAULT;
	}
	kfree(list);
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
		info.width         = g_active_w;
		info.height        = g_active_h;
		info.format        = VDRM_FMT_XRGB8888;
		info.pitch         = g_mode_pitch;
		info.pool_capacity = VDRM_POOL_MAX;
		info.flags         = VDRM_CAPS_ALL;
		pr_debug("vdrm: GET_INFO sizeof=%d abi=%u %ux%u fmt=%08x pitch=%u cap=%u arg=%px\n",
			(int)sizeof(struct vdrm_info), info.abi_version, info.width,
			info.height, info.format, info.pitch, info.pool_capacity,
			(void *)arg);
		if (copy_to_user((void *)arg, (const void *)&info, sizeof(info)))
			return -EFAULT;
		return 0;
	}
	case VDRM_IOCTL_GET_POOL:
		return vdrm_pool_list_ioctl((void __user *)arg,
					    VDRM_IOC_SIZE(cmd));
	case VDRM_IOCTL_REGISTER: {
		struct vdrm_register_req req;
		struct dma_buf *dbuf;
		struct drm_gem_object *obj;
		struct file *f0 = NULL, *f1 = NULL;
		unsigned long flags;
		int i, k, slot = -1;

		if (copy_from_user(&req, (const void *)arg, sizeof(req)))
			return -EFAULT;
		if (!g_dev)
			return -ENODEV;

		dbuf = dma_buf_get(req.fd);
		if (!dbuf || (unsigned long)dbuf > (unsigned long)-4096)
			return -EBADF;

		/* 幂等：同一个 dma_buf 已注册就直接返回原 id。先查一次省掉
		 * import；import 之后槽可能被别人占走，所以还要再查一次。*/
		spin_lock_irqsave(&g_pool_state_lock, flags);
		for (i = 0; i < VDRM_POOL_MAX; i++) {
			if (g_pool[i].used && g_pool[i].obj &&
			    g_pool[i].obj->import_attach &&
			    g_pool[i].obj->import_attach->dmabuf == dbuf) {
				req.id = g_pool[i].id;
				spin_unlock_irqrestore(&g_pool_state_lock, flags);
				dma_buf_put(dbuf);
				copy_to_user((char __user *)arg, &req, sizeof(req));
				return 0;
			}
		}
		spin_unlock_irqrestore(&g_pool_state_lock, flags);

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

		f0 = fget(req.fd);
		if (!f0) {
			vdrm_gem_put(obj);
			return -EBADF;
		}
		if (req.fd1 >= 0) {
			f1 = fget(req.fd1);
			if (!f1) {
				fput(f0);
				vdrm_gem_put(obj);
				return -EBADF;
			}
		}

		/* import/fget 期间槽可能被占走、整池可能被清空 —— 必须重新选
		 * 槽，并在同一把锁里"填字段 + 最后发布 used"。*/
		spin_lock_irqsave(&g_pool_state_lock, flags);
		for (i = 0; i < VDRM_POOL_MAX; i++) {
			if (g_pool[i].used && g_pool[i].obj &&
			    g_pool[i].obj->import_attach &&
			    g_pool[i].obj->import_attach->dmabuf ==
					obj->import_attach->dmabuf) {
				req.id = g_pool[i].id;
				spin_unlock_irqrestore(&g_pool_state_lock, flags);
				pr_info("vdrm: REGISTER dup -> id=%u\n", req.id);
				fput(f0);
				if (f1)
					fput(f1);
				vdrm_gem_put(obj);
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
		if (slot < 0) {
			spin_unlock_irqrestore(&g_pool_state_lock, flags);
			fput(f0);
			if (f1)
				fput(f1);
			vdrm_gem_put(obj);
			return -ENOSPC;
		}
		memset(&g_pool[slot], 0, sizeof(g_pool[slot]));
		g_pool[slot].obj       = obj;
		g_pool[slot].user_data = req.user_data;
		g_pool[slot].id        = g_pool_next_id++;
		g_pool[slot].width     = req.width;
		g_pool[slot].height    = req.height;
		g_pool[slot].format    = req.format;
		g_pool[slot].pitch     = req.pitch;
		g_pool[slot].state     = VDRM_ST_FREE;
		g_pool[slot].f0        = f0;
		g_pool[slot].f1        = f1;
		g_pool[slot].num_ints  = (req.num_ints <= VDRM_MAX_INTS) ?
			req.num_ints : VDRM_MAX_INTS;
		for (k = 0; k < (int)g_pool[slot].num_ints; k++)
			g_pool[slot].ints[k] = req.ints[k];
		req.id = g_pool[slot].id;
		/* 所有字段就绪后再发布 used：读侧看到 used=1 就是完整记录 */
		g_pool[slot].used = 1;
		spin_unlock_irqrestore(&g_pool_state_lock, flags);

		pr_info("vdrm: REGISTER id=%u %ux%u pitch=%u size=%zu\n",
			req.id, req.width, req.height, req.pitch, obj->size);
		/* 等在 dumb_create 里的合成器可以重新试了 */
		wake_up_interruptible(&g_pool_wq);
		if (copy_to_user((void *)arg, &req, sizeof(req))) {
			/* 回传失败：把刚入池的槽退掉 */
			spin_lock_irqsave(&g_pool_state_lock, flags);
			g_pool[slot].used = 0;
			spin_unlock_irqrestore(&g_pool_state_lock, flags);
			memset(&g_pool[slot], 0, sizeof(g_pool[slot]));
			fput(f0);
			if (f1)
				fput(f1);
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
		spin_lock_irqsave(&g_pool_state_lock, flags);
		for (i = 0; i < VDRM_POOL_MAX; i++) {
			if (g_pool[i].used && g_pool[i].id == req.id) {
				if (g_pool[i].state == VDRM_ST_DISPL)
					g_pool[i].state = VDRM_ST_FREE;
				/* 合成器已经销毁了 handle（旧尺寸 buffer 被丢弃），
				 * 这一帧也放完了：分配权现在可以真正放开。
				 * owner 为空 + handle_count 为 0 覆盖 postclose 的
				 * 残留场景（合成器退了，但 App 还没 RETURN）。*/
				if (g_pool[i].handle_gone ||
				    (!g_pool[i].allocation_owner && g_pool[i].obj &&
				     READ_ONCE(g_pool[i].obj->handle_count) == 0))
					vdrm_slot_release_allocation(i);
				/* 这里【不再】完成 flip-done。
				 *
				 * RETURN 的语义只有一条：SF 放手了，这块显存可以再分配。
				 * "这一帧什么时候算上屏完成"是另一件事，由 App 报的
				 * present fence 决定（见 PRESENTED）。以前两件事挤在
				 * 同一条路径上，等于让"SF 什么时候释放"兼职"什么时候
				 * 上屏"，慢的那条会把合成器一起拖住。
				 *
				 * 万一 present 永远不来，槽上的事件由 vblank 定时器里
				 * 的兜底超时完成，不会悬空。*/
				rc = 0;
				break;
			}
		}
		spin_unlock_irqrestore(&g_pool_state_lock, flags);
		if (rc == 0)
			wake_up_interruptible(&g_pool_wq);   /* 等清池的人在等这个 */
		return rc;
	}
	case VDRM_IOCTL_UNREGISTER: {
		struct vdrm_handle_req req;
		unsigned long flags;
		int i;

		if (copy_from_user(&req, (const void *)arg, sizeof(req)))
			return -EFAULT;

		/* 要清的范围里如果有槽正在显示/被合成器占着，先等一会儿：
		 * 运行时切分辨率就是这种情形 —— 合成器手里还攥着旧尺寸的槽，
		 * 它收到 hotplug 后会先关输出、放掉 GEM，然后我们才能清干净。
		 * 不等的话 mkpool 只会拿到 4/6，下一轮重试又会把池清空。*/
		{
			u32 waited = 0;
			bool busy;

			for (;;) {
				busy = false;
				spin_lock_irqsave(&g_pool_state_lock, flags);
				for (i = 0; i < VDRM_POOL_MAX; i++) {
					if (!g_pool[i].used)
						continue;
					if (req.id && g_pool[i].id != req.id)
						continue;
					if (g_pool[i].allocation_reserved ||
					    g_pool[i].state != VDRM_ST_FREE) {
						busy = true;
						break;
					}
				}
				spin_unlock_irqrestore(&g_pool_state_lock, flags);
				if (!busy)
					break;
				if (req.id != 0 || waited >= 3000)
					return -EBUSY;
				if (wait_event_interruptible_timeout(g_pool_wq, false,
								     msecs_to_jiffies(100)) < 0)
					return -ERESTARTSYS;
				waited += 100;
			}
			spin_lock_irqsave(&g_pool_state_lock, flags);
		}

		if (req.id == 0) {
			/* id=0：清空整池（mkpool 开跑前用，避免多次运行累积批次）*/
			struct file *p0[VDRM_POOL_MAX], *p1[VDRM_POOL_MAX];
			struct drm_gem_object *objs[VDRM_POOL_MAX];
			struct drm_pending_vblank_event *evs[VDRM_POOL_MAX];
			int n = 0;

			for (i = 0; i < VDRM_POOL_MAX; i++) {
				if (!g_pool[i].used)
					continue;
				g_pool[i].used = 0;
				p0[n]   = g_pool[i].f0;
				p1[n]   = g_pool[i].f1;
				objs[n] = g_pool[i].obj;
				memset(&g_pool[i], 0, sizeof(g_pool[i]));
				n++;
			}
			spin_unlock_irqrestore(&g_pool_state_lock, flags);
			/* 挂着等 RETURN 的 flip 事件不能丢：丢了合成器会永远等下去 */
			for (i = 0; i < n; i++) {
				if (p0[i])
					fput(p0[i]);
				if (p1[i])
					fput(p1[i]);
				if (objs[i])
					vdrm_gem_put(objs[i]);
			}
			pr_info("vdrm: UNREGISTER id=0 -> 清空整池\n");
			return 0;
		}

		{
			struct file *p0 = NULL, *p1 = NULL;
			struct drm_gem_object *po = NULL;
			int hit = -1;

			for (i = 0; i < VDRM_POOL_MAX; i++) {
				if (g_pool[i].used && g_pool[i].id == req.id) {
					hit = i;
					g_pool[i].used = 0;
					p0 = g_pool[i].f0;
					p1 = g_pool[i].f1;
					po = g_pool[i].obj;
					memset(&g_pool[i], 0, sizeof(g_pool[i]));
					break;
				}
			}
			spin_unlock_irqrestore(&g_pool_state_lock, flags);
			if (hit < 0)
				return -ENOENT;
			if (p0)
				fput(p0);
			if (p1)
				fput(p1);
			if (po)
				vdrm_gem_put(po);
			return 0;
		}
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
	case VDRM_IOCTL_SET_EVENTFD: {
		s32 ufd;
		struct eventfd_ctx *ctx = NULL, *old;
		unsigned long flags;

		if (copy_from_user(&ufd, (const void *)arg, sizeof(ufd))) {
			/* 老 App 把 eventfd 的 fd 值直接当 arg 传（不是指针）。
			 * 兼容它：copy 失败时按值解释。arg == 0 就是清除。*/
			ufd = (s32)arg;
		}
		if (ufd) {
			ctx = eventfd_ctx_fdget(ufd);
			if (IS_ERR(ctx))
				return PTR_ERR(ctx);
		}
		spin_lock_irqsave(&g_app_ctx_lock, flags);
		old = g_app_ctx;
		g_app_ctx = ctx;
		spin_unlock_irqrestore(&g_app_ctx_lock, flags);
		if (old)
			eventfd_ctx_put(old);
		pr_info("vdrm: app eventfd %s\n", ufd ? "registered" : "cleared");
		/* 第一次接上：把当前在屏的帧补推给它，App 重启/静态桌面不会黑屏。*/
		if (ctx && !old)
			vdrm_push_current_frame();
		return 0;
	}
	/* 现在只是**诊断**：记下 commit→present 的耗时，不再参与 flip-done
	 * （见 vdrm_pipe_update 里的结论）。present fence 仍然有价值 —— 它告诉
	 * 我们"这一帧什么时候真的上了屏"，只是不该拿它当节拍。*/
	case VDRM_IOCTL_PRESENTED: {
		struct vdrm_present_req req;
		struct drm_pending_vblank_event *pend = NULL;
		unsigned long flags;
		int i, rc = -ENOENT;

		if (copy_from_user(&req, (const void *)arg, sizeof(req)))
			return -EFAULT;
		spin_lock_irqsave(&g_pool_state_lock, flags);
		for (i = 0; i < VDRM_POOL_MAX; i++) {
			if (g_pool[i].used && g_pool[i].id == req.id) {
				u64 now = ktime_get_ns();

				if (g_pool[i].pend_since_ns) {
					u64 dt = (now - g_pool[i].pend_since_ns) / 1000;

					g_present_lat_us = dt;
					if (dt > g_present_lat_max_us)
						g_present_lat_max_us = dt;
				}
				/* safe_present：这一刻才让合成器往下走 */
				pend = g_pool[i].pend_event;
				g_pool[i].pend_event = NULL;
				if (pend) {
					g_flip_safe_present_done++;
					g_frame_flip_present_done++;
				}
				rc = 0;
				break;
			}
		}
		spin_unlock_irqrestore(&g_pool_state_lock, flags);
		if (pend)
			vdrm_flip_complete(pend);
		return rc;
	}
	/* App 回到前台："我回来了，把当前这一帧立刻重推给我。"
	 *
	 * 后台时 present 不来，帧事件就卡在 VDRM_FLIP_FALLBACK_MS 那一档上
	 * （桌面掉到 2fps）。这时候如果干等合成器下一次提交，屏幕上会先停一下
	 * 再"醒"过来 —— 那一眼就是用户看到的卡顿。这里主动推一次当前帧，
	 * App 立刻重贴最后一帧，恢复是瞬时的。
	 *
	 * 顺带给 vblank 预热几拍：合成器马上会重新提交，这几拍能让挂着的
	 * flip-done 及时完成。*/
	case VDRM_IOCTL_READY:
		vdrm_push_current_frame();
		g_vblank_kick_ticks = 3;
		vdrm_vblank_grid_start();
		vdrm_app_signal();
		pr_debug("vdrm: app ready -> push current frame\n");
		return 0;
	case VDRM_IOCTL_GET_CONF: {
		struct vdrm_conf c;

		if (!g_dev)
			return -ENODEV;
		memset(&c, 0, sizeof(c));
		c.width     = g_mode_w;
		c.height    = g_mode_h;
		c.screen_w  = g_screen_w;
		c.screen_h  = g_screen_h;
		c.mode_gen  = g_mode_gen;
		c.commit_gen = g_commit_gen;
		if (copy_to_user((void *)arg, &c, sizeof(c)))
			return -EFAULT;
		return 0;
	}
	case VDRM_IOCTL_SET_SCREEN: {
		struct vdrm_screen_req req;

		if (copy_from_user(&req, (const void *)arg, sizeof(req)))
			return -EFAULT;
		/* 只接受合理范围：和 mode 的上下限一致（16..4096）*/
		if (req.width < 16 || req.height < 16 ||
		    req.width > 4096 || req.height > 4096)
			return -EINVAL;
		if (g_screen_w == req.width && g_screen_h == req.height)
			return 0;
		g_screen_w = req.width;
		g_screen_h = req.height;
		vdrm_modes_rebuild();
		pr_debug("vdrm: App 上报屏幕 %ux%u -> 广告 %d 档分辨率\n",
			g_screen_w, g_screen_h, g_mode_count);
		/* 列表变了：如果合成器已经在跑，让它重新探测；启动阶段没有影响 */
		if (g_dev && g_registered)
			drm_kms_helper_hotplug_event(g_dev);
		return 0;
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
		/* 同上：GET_POOL 只认 dir/type/nr，不认结构大小 ——
		 * 老版本 App（结构小一点）也能正常读到池。*/
		if (VDRM_IOC_BASE(cmd) == VDRM_IOC_BASE(VDRM_IOCTL_GET_POOL))
			return vdrm_pool_list_ioctl((void __user *)arg,
						    VDRM_IOC_SIZE(cmd));
		pr_debug("vdrm: unknown ioctl %#x\n", cmd);
		return -ENOTTY;
	}
}

/* ---------------- 阶段实现（在 kworker 上下文里跑，可以睡眠）---------------- */

static void vdrm_drv_release(struct drm_device *dev);

/* ===== 分配占用的会话边界归还 =====
 *
 * 正常路径是 dumb_destroy（见上）：合成器销毁一块 dumb 缓冲，槽立刻可复用。
 * 这里只处理“合成器整个退掉、没走 dumb_destroy”的兜底：
 *   - TAKEN/FREE 槽：解除分配占用，置 FREE；
 *   - DISPL 槽：App 可能还在屏上显示，只记 handle_gone，等 App RETURN 再放开。
 *
 * 这和旧实现（会话内永不复用、只等 postclose）的区别：切分辨率时合成器销毁
 * 旧尺寸 buffer 后，旧槽马上回到池里，来回切不会把 32 个槽耗光。*/
static void vdrm_postclose(struct drm_device *dev, struct drm_file *file)
{
	unsigned long flags;
	bool released = false;
	int i;
	(void)dev;

	spin_lock_irqsave(&g_pool_state_lock, flags);
	for (i = 0; i < VDRM_POOL_MAX; i++) {
		struct vdrm_pool_entry *e = &g_pool[i];

		if (!e->used || e->allocation_owner != file)
			continue;
		if (e->state == VDRM_ST_DISPL) {
			e->allocation_owner = NULL;
			/* 只有确认对象上没有任何 handle 了，才允许 RETURN
			 * 顺手放开分配权；否则等最后一个 handle 销毁后由
			 * dumb_create 的懒回收处理。*/
			if (!e->obj || READ_ONCE(e->obj->handle_count) == 0)
				e->handle_gone = 1;
		} else if (!e->obj ||
			   READ_ONCE(e->obj->handle_count) == 0) {
			if (vdrm_slot_release_allocation(i))
				released = true;
		} else {
			/* 还有别的 handle（PRIME 导入等）活着：保留占用，
			 * 等它被销毁后 dumb_create 的懒回收会放开。*/
			e->allocation_owner = NULL;
		}
	}
	spin_unlock_irqrestore(&g_pool_state_lock, flags);
	if (released)
		wake_up_interruptible(&g_pool_wq);
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

	g_driver.driver_features = VDRM_DRIVER_FEATURES | DRIVER_RENDER;
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
	/* 分配占用的真正释放点：合成器销毁 dumb 缓冲时把槽放回可用集合。
	 * 不设它的话，一个槽在整场合成器会话里只能发一次，来回切分辨率会把
	 * 32 个槽耗光（dumb_create 6s 后 ENOSPC）。*/
#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 6, 0)
	/* 6.6 起 struct drm_driver 没有 dumb_destroy 了（DESTROY_DUMB 走
	 * drm_gem_handle_delete）。新内核上靠 vdrm_dumb_create 开头的
	 * vdrm_slot_reap_closed() 懒回收 + postclose 兜底 —— 效果一样，
	 * 只是"槽何时变空闲"晚一次 dumb_create。*/
	g_driver.dumb_destroy              = vdrm_dumb_destroy;
#endif
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
	struct file *p0[VDRM_POOL_MAX], *p1[VDRM_POOL_MAX];
	struct drm_gem_object *objs[VDRM_POOL_MAX];
	unsigned long flags;
	int i, n = 0;

	/* 顺序很重要：先断新入口（/dev/vdrm ioctl），再停 vblank 定时器
	 * （提交路径里唯一还在原子上下文碰池的东西），然后把在飞的提交
	 * 排干，最后才回收池。旧版先清池、后注销 DRM，pipe_update 会踩着
	 * 正在释放的池项。*/
	if (g_misc_registered) {
		misc_deregister(&g_misc);
		g_misc_registered = 0;
		pr_info("vdrm: /dev/vdrm deregistered\n");
	}

	if (g_vblank_timer_inited) {
		g_vblank_on = 0;
		g_vblank_timer_running = 0;
		hrtimer_cancel(&g_vblank_timer);
	}

	if (g_app_ctx) {
		struct eventfd_ctx *ctx;

		spin_lock_irqsave(&g_app_ctx_lock, flags);
		ctx = g_app_ctx;
		g_app_ctx = NULL;
		spin_unlock_irqrestore(&g_app_ctx_lock, flags);
		if (ctx)
			eventfd_ctx_put(ctx);
	}

	if (g_dev && g_registered) {
		/* 排干在飞的 atomic commit（会走 pipe_update，读池）*/
		drm_atomic_helper_shutdown(g_dev);
		drm_dev_unregister(g_dev);
		g_registered = 0;
		pr_info("vdrm: unregistered\n");
	}

	/* 到这里不会再有提交/ioctl 访问池，可以安全回收 */
	spin_lock_irqsave(&g_pool_state_lock, flags);
	for (i = 0; i < VDRM_POOL_MAX; i++) {
		if (!g_pool[i].used)
			continue;
		g_pool[i].used = 0;
		p0[n]   = g_pool[i].f0;
		p1[n]   = g_pool[i].f1;
		objs[n] = g_pool[i].obj;
		memset(&g_pool[i], 0, sizeof(g_pool[i]));
		n++;
	}
	spin_unlock_irqrestore(&g_pool_state_lock, flags);
	for (i = 0; i < n; i++) {
		if (p0[i])
			fput(p0[i]);
		if (p1[i])
			fput(p1[i]);
		if (objs[i])
			vdrm_gem_put(objs[i]);
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

	/* 网格连续前进：没人持 vblank 引用时也照常走，只是不上报。
	 * kick 期间例外 —— 那几次要真的上报，合成器才会看到"新的一帧时刻"。*/
	if (g_vblank_on || g_vblank_kick_ticks)
		drm_crtc_handle_vblank(&g_pipe->crtc);
	g_vblank_ticks++;

	/* 预热只递减，不要在这里停网格。
	 *
	 * 旧写法一到期就 vdrm_vblank_grid_stop()，完全没看还有没有挂起的
	 * flip-done。正常路径下挂起事件会持 vblank 引用（g_vblank_on=1），碰不到；
	 * 但换档时 pipe_disable 已经把 g_vblank_on 清 0，旧槽上的事件还在 ——
	 * 新档 enable 的 3 拍预热一用完就停网格，事件连 100ms 兜底都没了。
	 * 停不停统一交给下面 !left && !g_vblank_on && !kick 那套条件。*/
	if (g_vblank_kick_ticks)
		g_vblank_kick_ticks--;

	/* 完成挂起的 flip-done。两条路取先到者：
	 *   1. App 报了 present —— 但那条走 ioctl 就地完成，不在这个 tick 里；
	 *   2. 兜底超时（present 不来、或 App 挂掉时仍能跑）。
	 *
	 * 现在这个 tick 只做一件事：没有需要 vblank 的人了就把网格停掉。
	 * flip-done 由 DRM core 的 vblank_event_list 在 handle_vblank 里发完，
	 * 它自己会 put 掉引用（g_vblank_on 归 0），所以这里只看那两个条件。*/
	{
		/* 保守模式的兜底：present 一直不来就自己完成（否则合成器永远等）*/
		struct drm_pending_vblank_event *exp[VDRM_POOL_MAX];
		u64 lim = (u64)VDRM_SAFE_FALLBACK_MS * 1000000ULL;
		unsigned long fl;
		int k, n = 0;

		if (g_safe_present) {
			u64 now = ktime_get_ns();

			spin_lock_irqsave(&g_pool_state_lock, fl);
			for (k = 0; k < VDRM_POOL_MAX; k++) {
				if (!g_pool[k].pend_event)
					continue;
				if (now - g_pool[k].pend_since_ns <= lim)
					continue;
				exp[n++] = g_pool[k].pend_event;
				g_pool[k].pend_event = NULL;
				g_flip_safe_fallback++;
			}
			spin_unlock_irqrestore(&g_pool_state_lock, fl);
			for (k = 0; k < n; k++)
				vdrm_flip_complete(exp[k]);
		}
	}

	if (!g_vblank_on && !g_vblank_kick_ticks)
		vdrm_vblank_grid_stop();

	/* 用绝对时间推进，停机再开能接回原相位 */
	g_vblank_next = ktime_add_ns(g_vblank_next, (u64)g_vblank_period_ns);
	hrtimer_set_expires(t, g_vblank_next);

	/* 回调里可能刚被 disable_vblank 停掉（最后一个 flip 事件就是在这次
	 * handle_vblank 里发完、put 掉引用的），那就别再来一圈。*/
	if (!g_vblank_timer_running)
		return HRTIMER_NORESTART;
	return HRTIMER_RESTART;
}

/* 起网格：落到原来的相位上。没人需要 vblank 时网格是停的，所以每次 flip
 * 的 get(0->1) 都会重新起一次 —— 相位的连续性靠 g_vblank_next 维持。*/
static void vdrm_vblank_grid_start(void)
{
	ktime_t now;
	u64 period = (u64)g_vblank_period_ns;

	if (!g_vblank_timer_inited || period == 0)
		return;
	if (g_vblank_timer_running)
		return;

	now = ktime_get();
	if (ktime_before(g_vblank_next, now)) {
		s64 d = ktime_to_ns(ktime_sub(now, g_vblank_next));
		s64 k = div_s64(d, (s64)period) + 1;

		g_vblank_next = ktime_add_ns(g_vblank_next, (u64)k * period);
	}
	g_vblank_timer_running = 1;
	g_vblank_running_view = 1;
	hrtimer_start(&g_vblank_timer, g_vblank_next, HRTIMER_MODE_ABS);
}

/* 停网格：只从"还有没有人要 vblank"的角度停，不重置相位。
 * 可能从 disable_vblank 里被调用（持 event_lock + vbl_lock），所以只能用
 * 不等待的 try_to_cancel —— 回调在别的 CPU 上跑着时它会自己看到
 * g_vblank_timer_running==0 然后不再重启。*/
static void vdrm_vblank_grid_stop(void)
{
	g_vblank_timer_running = 0;
	g_vblank_running_view = 0;
	hrtimer_try_to_cancel(&g_vblank_timer);
}

/* 由 DRM core 在需要 vblank 中断时调用（drv_vblank_get 的 0->1） */
static int vdrm_pipe_enable_vblank(struct drm_simple_display_pipe *pipe)
{
	if (!g_vblank_timer_inited)
		return -EINVAL;

	g_vblank_on = 1;
	vdrm_vblank_grid_start();
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
	/* 没人等了就把网格停掉：空闲时不再每秒醒 120 次。
	 * 相位留在 g_vblank_next 里，下次 get 时接回原节拍。*/
	vdrm_vblank_grid_stop();
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
	/* 合成器真正编程进去的尺寸 = 池/GET_INFO 要跟随的尺寸。*/
	if (m->hdisplay && m->vdisplay &&
	    (g_active_w != m->hdisplay || g_active_h != m->vdisplay)) {
		g_active_w = m->hdisplay;
		g_active_h = m->vdisplay;
		/* 换档真正完成的这一刻再点一次 vblank 预热（commit 时那一次在
		 * modeset 之前，合成器还没换完）。这几拍让它立刻重画一帧新尺寸的
		 * 画面，App 才不会停在上一个尺寸的 letterbox 上。*/
		g_vblank_kick_ticks = 3;
		vdrm_vblank_grid_start();
		/* 新档生效。
		 *
		 * 注意顺序：合成器这次 modeset 提交的 pipe_update 已经**先**把新尺寸
		 * 那一帧推进队列了（commit_planes 在 commit_modeset_enables 之前），
		 * 所以这里不能清队列 —— 清了就等于把"新档的第一帧"扔掉，而静态桌面
		 * 换完档不会再有新提交，App 只能一直贴着上一档的旧帧（尾帧）。
		 *
		 * 旧尺寸的帧不用怕：App 那边按 slot id 过滤，没 forge 过的 id 直接丢弃，
		 * 而换档后它只会 forge 新尺寸的槽。*/
		wake_up_interruptible(&g_pool_wq);
		vdrm_app_signal();
		/* 再补推一次当前帧：App 可能还没 forge 完新尺寸的槽（上面那帧因此被
		 * 当成 unknown slot 丢掉了），它 rebuild 完之后会来一发 READY，那次才
		 * 真正接上。这里先推是给"已经 forge 好"的情况省一次往返。*/
		vdrm_push_current_frame();
		/* 这里【不】回收旧档 —— 曾经在这里调过 vdrm_pool_drop_stale()，
		 * 但它要 fput + drm_gem_object_put，底层会走到 dma-buf 释放、甚至
		 * vendor allocator 的 HAL 回调，而这里是**合成器的 modeset 提交路径**，
		 * 卡住就等于卡住那一帧 —— 实测换档瞬间会顿一下。
		 * 回收改到 dumb_create 开头（进程上下文的普通路径）就够了：每次换档
		 * 合成器必然先来要新尺寸的 buffer。*/
	}
	/* 相位起点：从"现在 + 一个周期"开始，和旧行为一致（首帧不会立刻完成）*/
	g_vblank_next = ktime_add_ns(ktime_get(), (u64)g_vblank_period_ns);

	pr_info("vdrm: pipe enable %ux%u period=%llu ns\n",
		m->hdisplay, m->vdisplay,
		(unsigned long long)g_vblank_period_ns);

	/* 通知 DRM core vblank 硬件已上线 */
	drm_crtc_vblank_on(&pipe->crtc);
}

static void vdrm_pipe_disable(struct drm_simple_display_pipe *pipe)
{
	struct drm_pending_vblank_event *pend[VDRM_POOL_MAX];
	unsigned long flags;
	int i, n = 0;

	pr_info("vdrm: pipe disable\n");

	/* 默认模式下 flip-done 是"arm 到下一个 vblank"，由 DRM core 自己的
	 * vblank_event_list 管着，drm_crtc_vblank_off() 会收尾。但 safe_present
	 * 模式下事件是私挂在槽上的，DRM 看不到 —— 停网格之前必须自己补发，
	 * 否则合成器会一直等上一帧，热插拔后再也不提交。*/
	if (g_safe_present) {
		struct drm_pending_vblank_event *pev[VDRM_POOL_MAX];
		unsigned long fl;
		int k, n = 0;

		spin_lock_irqsave(&g_pool_state_lock, fl);
		for (k = 0; k < VDRM_POOL_MAX; k++) {
			if (!g_pool[k].pend_event)
				continue;
			pev[n++] = g_pool[k].pend_event;
			g_pool[k].pend_event = NULL;
		}
		spin_unlock_irqrestore(&g_pool_state_lock, fl);
		for (k = 0; k < n; k++)
			vdrm_flip_complete(pev[k]);
	}

	/* 旧档的帧事件也不再需要 */
	vdrm_frame_clear();

	g_vblank_kick_ticks = 0;   /* CRTC 已关，别再让预热拍 vblank */
	g_vblank_on = 0;
	vdrm_vblank_grid_stop();
	drm_crtc_vblank_off(&pipe->crtc);
}

/*
 * M4 核心：flip-done 门控。
 * drm_simple_kms_helper.h 对 .update 的契约是——驱动必须在这里提交
 * crtc_state->event。有 vblank 时用 arm（事件在下一个 vblank 才发，天然实现
 * “帧真正上屏后才通知”）；拿不到 vblank 引用则退化为立即 send。
 * 这就是合成器依赖的 page-flip 完成回调。
 */
/* 等合成器的 GPU 画完，再让这块 buffer 上屏。
 *
 * 之前完全没有 fence 处理：合成器把 IN_FENCE_FD（显式）或者 dma-buf 的隐式
 * reservation fence 交上来，我们一个都没等就 pipe_update 里"上屏"了。静态画面
 * 看不出来（GPU 早就画完了），一放动画就是**每帧都可能扫到画了一半的内容** ——
 * 屏幕上表现为一直在闪/撕。
 *
 * drm_gem_simple_display_pipe_prepare_fb() 只负责把 fence 收进 plane_state->fence，
 * 真正的等待要驱动自己做（这也是 drm_gem_plane_helper_prepare_fb 的文档要求）。
 * prepare_fb 跑在提交路径的进程上下文，可以睡。
 *
 * 加超时是保险：fence 永远不来时不能让整个提交挂死。*/
/* 诊断：合成器交上来的 fence 等了多久、有没有等超时。
 * "新窗口首帧闪一下"如果伴随 fence_timeouts 增长，就实锤是 GPU 没画完
 * 就被扫描输出了（录屏抓不到，因为录屏读的是画完之后的内容）。*/
static unsigned long long g_fence_waits, g_fence_timeouts;
static unsigned long long g_fence_max_us, g_fence_last_us;
module_param_named(fence_waits, g_fence_waits, ullong, 0444);
module_param_named(fence_timeouts, g_fence_timeouts, ullong, 0444);
module_param_named(fence_max_us, g_fence_max_us, ullong, 0444);
module_param_named(fence_last_us, g_fence_last_us, ullong, 0444);

static int vdrm_pipe_prepare_fb(struct drm_simple_display_pipe *pipe,
				struct drm_plane_state *new_state)
{
	int ret;

	/* 设备内核只导出 drm_gem_plane_helper_prepare_fb（simple 那层没导出），
	 * 效果一样：它把显式 fence 和 dma-buf 的隐式 fence 串进 new_state->fence。*/
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 15, 0)
	ret = drm_gem_plane_helper_prepare_fb(&pipe->plane, new_state);
#else
	/* 5.10：语义一样（把显式 fence + dma-buf 隐式 fence 串进
	 * new_state->fence），只是名字/头不同。*/
	ret = drm_gem_fb_prepare_fb(&pipe->plane, new_state);
#endif
	if (ret)
		return ret;

	if (new_state->fence) {
		u64 t0 = ktime_get_ns();
		long w = dma_fence_wait_timeout(new_state->fence, false,
						msecs_to_jiffies(100));
		u64 dt_us = (ktime_get_ns() - t0) / 1000;

		g_fence_waits++;
		g_fence_last_us = dt_us;
		if (dt_us > g_fence_max_us)
			g_fence_max_us = dt_us;
		if (w == 0) {
			g_fence_timeouts++;
			pr_info_ratelimited("vdrm: 等 GPU fence 超时(%lluus)，先上屏\n",
					    dt_us);
		} else if (w < 0) {
			return (int)w;
		}
	}
	return 0;
}

static void vdrm_pipe_update(struct drm_simple_display_pipe *pipe,
			     struct drm_plane_state *old_state)
{
	struct drm_crtc *crtc = &pipe->crtc;
	struct drm_crtc_state *crtc_state = crtc->state;
	struct drm_pending_vblank_event *event;
	/* 在拿锁之前取时间：ktime_get_ns 内部有 seqcount 重试，放锁里
	 * 只会延长关中断时间，没有别的好处。*/
	u64 now_ns = ktime_get_ns();
	int slot_idx = -1;
	bool tail_frame = false;

	/* 换档那一拍：plane 上还是旧尺寸的 fb（合成器通常是先 modeset、下一帧
	 * 才换 buffer）。这不是“新档的第一帧”，只是指向上一个模式的尾帧：
	 *   - 推给 App，它会按新窗口几何把旧画面贴出去（CHANGES 里“旋转回滚”
	 *     的那条路）；
	 *   - 还会把这一拍的 flip-done 挂到旧槽上，平白增加关 CRTC 时事件悬空
	 *     的概率。
	 * 判断收得很窄：确实是一次 modeset，且 fb 尺寸和新 mode 对不上。
	 * 尺寸一致的“重新 enable”不算尾帧，照常推。不要求 old_state->fb
	 * 相同：disable→enable 的换档里 old fb 可能是 NULL，但尺寸对不上的
	 * 帧同样不能贴到新档上。*/
	if (crtc_state && crtc_state->mode_changed &&
	    pipe->plane.state && pipe->plane.state->fb &&
	    (pipe->plane.state->fb->width != crtc_state->mode.hdisplay ||
	     pipe->plane.state->fb->height != crtc_state->mode.vdisplay))
		tail_frame = true;

	/* M5: 池 buffer 上屏 -> 标 DISPLAYING + 通知 App。
	 * 注意：池对象是 import 来的，PRIME_HANDLE_TO_FD 会返回 App 原始 dma-buf，
	 * 合成器再 FD_TO_HANDLE 会得到“第二个 GEM”，所以不能比指针，要比
	 * 底层的 import_attach->dmabuf。 */
	if (!tail_frame && pipe->plane.state && pipe->plane.state->fb) {
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
			struct drm_gem_object *po;
			struct dma_buf *pdb;

			/* used 必须最先读：UNREGISTER/卸载会在同锁内置 used=0
			 * 后清字段，先读 obj 会读到半清空的记录。*/
			if (!g_pool[i].used)
				continue;
			po = g_pool[i].obj;
			pdb = (po && po->import_attach) ?
				po->import_attach->dmabuf : NULL;
			if (po == gobj || (fbdb && pdb && fbdb == pdb)) {
				int j;

				/* 这块槽上一次被扫描输出后，App 还没 RETURN（state 仍是
				 * DISPL）—— 合成器已经在往同一片显存里画了。记一笔。*/
				if (g_pool[i].state == VDRM_ST_DISPL)
					g_frames_reuse++;
				g_pool[i].state = VDRM_ST_DISPL;

				/* 别的 DISPL 槽：**不在这里回收**。
				 *
				 * 旧写法是"新帧一上屏，别的 DISPL 槽必然不在屏上了，
				 * 直接放回 FREE"。这个假设在 HWC overlay 下是错的：
				 * SF 把 App 的 layer 交给 DPU 直接扫描之后，还要多攥
				 * 1–2 帧才 release。内核提前放回 → 合成器立刻把下一帧
				 * 画进同一片显存 → 屏幕上正在扫的区域被改写，表现为
				 * "局部蓝块"，而录屏走另一条合成路径，抓不到。
				 *
				 * 归还的权威信号只有一个：App 的 RETURN（它挂在 SF 的
				 * release 回调上）。分配占用的回收在 dumb_destroy/
				 * GEM_CLOSE，和显示占用是两条独立的线 —— 这里不碰
				 * 别的 DISPL 槽，也不做“超时回收”。
				 */
				(void)j;
				ready_id = g_pool[i].id;
				ready_ud = g_pool[i].user_data;
				slot_idx = i;
				ready = true;
				break;
			}
		}
		spin_unlock_irqrestore(&g_pool_state_lock, flags);

		if (ready) {
			g_frames_pushed++;
			vdrm_frame_push(ready_id, ready_ud);
			pr_debug("vdrm: frame ready id=%u\n", ready_id);
		} else {
			g_frames_unmatched++;
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

	/* 这一帧的 flip-done 要不要等 SF 放手？
	 *
	 * 合成器的 buffer 循环是"flip done 即空闲"：一到 flip-done 它就敢
	 * 再往同一片显存里画下一帧。可我们的链路里还夹着一层 SurfaceFlinger
	 * + HWC：它把这块 buffer 锁进 overlay 平面之后要等下一次合成才
	 * release（三缓冲下再多占 1–2 个 vsync）。flip-done 若按传输完成就
	 * 发，合成器会立刻覆盖屏幕上正在扫描的显存 —— 实测就是"局部蓝块"，
	 * 且录屏抓不到（录屏走另一条合成路径，读的是覆盖之后的像素）。
	 *
	 * 【结论】flip-done 只 arm 到下一个 vblank，**不等 App 的 present**。
	 * 参考 anland 6.2 的实测（awl_sc.cpp 头部注释）：把"帧完成"从 SF 的
	 * OnComplete 发出去，一个按 frame-callback 跑的合成器只有 29fps；改成
	 * 在 vsync 那一拍发就正常了。我们量到的 30fps / 偶发 107ms 是同一个坑
	 * —— present fence 是这条链里最慢的一环，不该让它决定帧率。
	 *
	 * 那"SF 还在显示这块显存"怎么办？靠池的回收链：槽在 App RETURN（SF 的
	 * release 信号）之前一直是 DISPL，dumb_create 不会把它发给别人。
	 * 唯一残余风险是合成器重提交同一块 fb —— 用 frames_reuse 盯着。*/
	if (slot_idx >= 0) {
		unsigned long fl;
		struct drm_pending_vblank_event *old = NULL;

		spin_lock_irqsave(&g_pool_state_lock, fl);
		g_pool[slot_idx].pend_since_ns = now_ns;
		if (g_safe_present && drm_crtc_vblank_get(crtc) == 0) {
			/* 保守模式：挂起来等 App present（同一槽被重提交时先补发旧的）*/
			old = g_pool[slot_idx].pend_event;
			g_pool[slot_idx].pend_event = event;
			event = NULL;
		}
		spin_unlock_irqrestore(&g_pool_state_lock, fl);
		if (old)
			vdrm_flip_complete(old);
		if (!event) {
			vdrm_vblank_grid_start();
			pr_debug("vdrm: flip held slot %d (safe_present)\n", slot_idx);
			return;
		}
	}

	_raw_spin_lock_irq(&crtc->dev->event_lock.rlock);
	/* 没有池槽可挂（HEAP 回退的那批 buffer，或者 vblank 引用拿不到）：
	 * arm 到下一个 vblank；连 vblank 都没有就立即完成。
	 * 立即路径不取 vblank 引用，事件由 DRM 在 send 时释放。*/
	if (drm_crtc_vblank_get(crtc) != 0)
		drm_crtc_send_vblank_event(crtc, event);
	else
		drm_crtc_arm_vblank_event(crtc, event);
	_raw_spin_unlock_irq(&crtc->dev->event_lock.rlock);

	pr_debug("vdrm: pipe update -> event armed\n");
}

/* mode 的时序参数：get_modes 造 mode、合成 EDID 都用同一份，别分叉。
 * 1920x1080@60 代进去就是 clock=148500 / htotal=2200 / vtotal=1125，
 * 和标准 CVT 一致。*/
struct vdrm_timing {
	u32 clock;              /* kHz */
	u16 hdisplay, hsync_start, hsync_end, htotal;
	u16 vdisplay, vsync_start, vsync_end, vtotal;
};

static void vdrm_timing_fill(struct vdrm_timing *t, u32 w, u32 h, u32 refresh)
{
	t->hdisplay    = w;
	t->hsync_start = w + 88;
	t->hsync_end   = w + 88 + 44;
	t->htotal      = w + 280;
	t->vdisplay    = h;
	t->vsync_start = h + 4;
	t->vsync_end   = h + 4 + 5;
	t->vtotal      = h + 45;
	t->clock       = (u32)div_u64((u64)t->htotal * t->vtotal * refresh, 1000);
}

/* ===== 合成一份 128 字节 EDID =====
 *
 * 虚拟 connector 默认没有 EDID，合成器拿不到物理尺寸/型号，日志里一直喊
 * `error getting EDID info for Virtual-…: no EDID property`。给一份最小的
 * base block：一个 detailed timing（就是我们唯一的那个 mode）、range limits、
 * 产品名/序列号。时序数字来自 vdrm_timing_fill()，所以 EDID 里的 preferred
 * timing 和 driver 报的 mode 严格一致，合成器不会因此选到别的分辨率。
 *
 * 注意：DRM_MODE_CONNECTOR_VIRTUAL 在 drm_connector_init() 里**不会**自动挂
 * EDID 属性（core 只给非 virtual/writeback 的挂），所以要自己 attach 一次。*/
#define VDRM_EDID_LEN 128

static void vdrm_edid_put_text(u8 *d, u8 tag, const char *txt)
{
	int i, n = strlen(txt);

	d[0] = 0; d[1] = 0; d[2] = 0; d[3] = tag; d[4] = 0;
	for (i = 0; i < 13; i++)
		d[5 + i] = (i < n) ? (u8)txt[i] :
			   (i == n ? 0x0A : 0x20);
}

static void vdrm_conn_set_edid(struct drm_connector *conn)
{
	struct vdrm_timing t;
	u8 e[VDRM_EDID_LEN];
	u32 hblank, vblank, hfront, hsyncw, vfront, vsyncw;
	u32 w_mm = 160;                 /* 16cm 宽，按 mode 长宽比推高度 */
	u32 h_mm;
	u32 hmax;
	int i, sum;

	memset(e, 0, sizeof(e));
	vdrm_timing_fill(&t, g_mode_w, g_mode_h, g_refresh);

	hblank = t.htotal - t.hdisplay;
	vblank = t.vtotal - t.vdisplay;
	hfront = t.hsync_start - t.hdisplay;
	hsyncw = t.hsync_end - t.hsync_start;
	vfront = t.vsync_start - t.vdisplay;
	vsyncw = t.vsync_end - t.vsync_start;
	if (g_edid_dpi > 0)             /* mm = px / dpi * 25.4 */
		w_mm = div_u64((u64)t.hdisplay * 2540, (u64)g_edid_dpi * 100);
	h_mm   = w_mm * t.vdisplay / t.hdisplay;
	hmax   = div_u64((u64)t.clock * 1000, t.htotal);   /* 行频 Hz */

	/* 0  header */
	e[0] = 0x00; e[1] = 0xFF; e[2] = 0xFF; e[3] = 0xFF;
	e[4] = 0xFF; e[5] = 0xFF; e[6] = 0xFF; e[7] = 0x00;

	/* 8  manufacturer id: 'V','D','R' 各 5 bit，大端 */
	e[8]  = (u8)(((('V' - 'A' + 1) & 0x1f) << 2) | ((('D' - 'A' + 1) & 0x1f) >> 3));
	e[9]  = (u8)(((('D' - 'A' + 1) & 0x1f) << 5) | (('R' - 'A' + 1) & 0x1f));

	/* 10 product code / 12 serial number（小端）*/
	e[10] = 0x01; e[11] = 0x12;
	e[12] = 0x01; e[13] = 0x00; e[14] = 0x00; e[15] = 0x00;

	/* 16 week / 17 year(-1990) */
	e[16] = 1;
	e[17] = 2026 - 1990;

	/* 18 EDID 1.4 / 20 数字输入 8bpc */
	e[18] = 1; e[19] = 4; e[20] = 0x80;

	/* 21/22 物理尺寸（cm）/ 23 gamma 2.2 / 24 特性：RGB + preferred timing */
	e[21] = (u8)(w_mm / 10);
	e[22] = (u8)(h_mm / 10);
	e[23] = 0x78;
	e[24] = 0x0A;

	/* 25..34 sRGB 色度坐标（标准值） */
	{
		static const u8 chroma[10] = {
			0xEE, 0x91, 0xA3, 0x54, 0x4C,
			0x99, 0x26, 0x0F, 0x50, 0x54,
		};
		memcpy(&e[25], chroma, sizeof(chroma));
	}

	/* 35..37 established timings 空，38..53 standard timings 全 0x0101 */
	for (i = 38; i < 54; i += 2) { e[i] = 0x01; e[i + 1] = 0x01; }

	/* 54 detailed timing descriptor */
	e[54] = (u8)(t.clock / 10);            /* 10 kHz 单位 */
	e[55] = (u8)((t.clock / 10) >> 8);
	e[56] = (u8)t.hdisplay;
	e[57] = (u8)hblank;
	e[58] = (u8)(((t.hdisplay >> 8) & 0x0f) << 4 | ((hblank >> 8) & 0x0f));
	e[59] = (u8)t.vdisplay;
	e[60] = (u8)vblank;
	e[61] = (u8)(((t.vdisplay >> 8) & 0x0f) << 4 | ((vblank >> 8) & 0x0f));
	e[62] = (u8)hfront;
	e[63] = (u8)hsyncw;
	e[64] = (u8)((vfront & 0x0f) | ((vsyncw & 0x0f) << 4));
	e[65] = (u8)((((hfront >> 8) & 0x03) << 6) | (((hsyncw >> 8) & 0x03) << 4) |
		     (((vfront >> 4) & 0x03) << 2) | (((vsyncw >> 4) & 0x03)));
	e[66] = (u8)w_mm;
	e[67] = (u8)h_mm;
	e[68] = (u8)(((w_mm >> 8) & 0x0f) << 4 | ((h_mm >> 8) & 0x0f));
	e[69] = 0; e[70] = 0;
	e[71] = 0x1E;                          /* 数字分离同步，+H +V */

	/* 72 display range limits */
	e[75] = 0xFD; e[76] = 0x00;
	e[77] = 48;                            /* min vrate */
	e[78] = (u8)(g_refresh > 255 ? 255 : g_refresh);
	e[79] = 30;                            /* min hrate */
	e[80] = (u8)(hmax / 1000 > 255 ? 255 : hmax / 1000);
	e[81] = (u8)((t.clock / 10000) + 1);   /* max pixel clock，10MHz 单位 */
	e[82] = 0x0A;                          /* CVT */
	for (i = 83; i < 90; i++) e[i] = 0x20;

	/* 90/108 产品名 + 序列号 */
	vdrm_edid_put_text(&e[90], 0xFC, "VDRM");
	vdrm_edid_put_text(&e[108], 0xFF, "vdrm-virtual");

	/* 126 无扩展块 / 127 校验和 */
	e[126] = 0;
	for (i = 0, sum = 0; i < 127; i++)
		sum += e[i];
	e[127] = (u8)(0x100 - (sum & 0xFF));

	drm_connector_update_edid_property(conn, (const struct edid *)e);
}

/* 同一分辨率下对外报几档刷新率。分辨率不变，所以池/App 完全不受影响 ——
 * 合成器可以在运行时切刷新率（niri: `niri msg output Virtual-3 mode ...`）。
 * 这不只是好看：120 档适合动画，60 档省电。*/
static const u32 g_refresh_list[] = { 0 /* 占位，实际由 g_refresh 补进来 */, 120, 90, 60 };

static int vdrm_conn_get_modes(struct drm_connector *connector)
{
	struct drm_display_mode *m;
	u32 rl[4];
	int rn = 0, i, r, added = 0;

	/* 去重后的刷新率列表：配置的那档放最前面 */
	rl[rn++] = g_refresh;
	for (i = 1; i < (int)ARRAY_SIZE(g_refresh_list); i++) {
		u32 x = g_refresh_list[i];
		int j, dup = 0;

		for (j = 0; j < rn; j++)
			if (rl[j] == x)
				dup = 1;
		if (!dup && rn < (int)ARRAY_SIZE(rl))
			rl[rn++] = x;
	}

	/* 每个分辨率 x 每档刷新率都给一个 mode。合成器在这组里挑，切换就是
	 * 一次普通 modeset —— 不需要 hotplug，也不需要重启。*/
	for (i = 0; i < g_mode_count; i++) {
		u32 w = g_modes[i][0];
		u32 h = g_modes[i][1];

		for (r = 0; r < rn; r++) {
			struct vdrm_timing t;

			m = vdrm_kzalloc(sizeof(*m));
			if (!m)
				continue;
			vdrm_timing_fill(&t, w, h, rl[r]);
			m->clock       = (int)t.clock;
			m->hdisplay    = t.hdisplay;
			m->hsync_start = t.hsync_start;
			m->hsync_end   = t.hsync_end;
			m->htotal      = t.htotal;
			m->vdisplay    = t.vdisplay;
			m->vsync_start = t.vsync_start;
			m->vsync_end   = t.vsync_end;
			m->vtotal      = t.vtotal;
			m->flags       = VDRM_MODE_FLAG_PHSYNC | VDRM_MODE_FLAG_PVSYNC;
			/* type 的位别记反：DRIVER=bit6，PREFERRED=bit3。
			 * 写反的后果实测是"屏幕全黑"——每档都带上 PREFERRED，
			 * 合成器按"第一个 preferred"挑，会挑到像素时钟最大的
			 * 全屏档，而池里只有配置那档的槽 → 分配失败 → 连接器
			 * 一直是 Disabled（niri: "error connecting connector"）。
			 * 只给配置的那一档（且是首档刷新率）标 PREFERRED。*/
			m->type        = VDRM_MODE_TYPE_DRIVER;
			if (w == g_mode_w && h == g_mode_h && r == 0)
				m->type |= VDRM_MODE_TYPE_PREFERRED;  /* 配置的那档 */

			if (m->htotal < w) m->htotal = w + 160;
			if (m->vtotal < h) m->vtotal = h + 45;
			drm_mode_set_name(m);
			drm_mode_probed_add(connector, m);
			added++;
		}
	}

	pr_debug("vdrm: get_modes -> %d (%d 个分辨率 x %d 档刷新率, preferred %ux%u@%u)\n",
		added, g_mode_count, rn, g_mode_w, g_mode_h, g_refresh);
	return added;
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
	g_pipe_funcs.prepare_fb             = vdrm_pipe_prepare_fb;
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

	/* 虚拟 connector 默认不挂 EDID 属性（core 只给非 virtual/writeback 的挂，
	 * 见 drm_connector_init()），这里自己挂上并塞一份合成 EDID。*/
	drm_connector_attach_edid_property(g_conn);
	vdrm_conn_set_edid(g_conn);

	rc = drm_simple_display_pipe_init(g_dev, g_pipe, &g_pipe_funcs,
					    g_formats,
					    sizeof(g_formats) / sizeof(g_formats[0]),
					    g_formats_mod, g_conn);
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

static bool vdrm_key_system(u32 code);

static struct input_dev *vdrm_input_dev_for(u32 type, u32 code)
{
	if (type == VDRM_EV_REL)
		return g_ptr;
	if (type == VDRM_EV_KEY) {
		/* 系统键（电源/休眠/唤醒…）一律不认：容器里的合成器不该能通过
		 * 这条通路合成一次"电源键按下"。真按电源键的是手机自己那颗
		 * pmic_pwrkey，走到 Android 的 PhoneWindowManager 里。*/
		if (vdrm_key_system(code)) {
			/* 静默丢掉会让"我先按了却没反应"变成谜案，留一行限流日志。*/
			pr_info_ratelimited("vdrm: 拒绝注入系统键 %u（电源/休眠类不走这条通路）\n",
					    code);
			return NULL;
		}
		return (code >= VDRM_BTN_FIRST && code <= VDRM_BTN_LAST) ?
			g_ptr : g_kbd;
	}
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

/** 这些是"系统键"，不能出现在虚拟键盘的能力表里。
 *
 * 一条只给容器用的键盘，凭什么声明自己能出 KEY_POWER？Android 的 InputReader
 * 打不开我们的节点（节点已经被换成 0600 root:root，实测 system_server 只开着
 * 真机的 event0~6/10~12），但**内核侧**的键处理器是按设备挂的：这台机上
 * /proc/bus/input/devices 里 vdrm-kbd 的 handlers 就带着 OnePlus 的
 * `oplus_keyevent_notifier`。一个"也声称能出电源键"的新设备会进它的电源键
 * 手势状态机（长按/连击判定），电源键的时长/次数就可能被它算歪。
 *
 * 所以能力表里把这些键摘掉：我们从来不发它们（App 的键表、模块脚本都查过，
 * 没有 116），摘掉只有好处。*/
static bool vdrm_key_system(u32 code)
{
	switch (code) {
	case 99:    /* KEY_SYSRQ */
	case 113:   /* KEY_MUTE（音量键家族，OEM 组合键状态机会读）*/
	case 114:   /* KEY_VOLUMEDOWN */
	case 115:   /* KEY_VOLUMEUP */
	case 116:   /* KEY_POWER */
	case 142:   /* KEY_SLEEP */
	case 143:   /* KEY_WAKEUP */
	case 205:   /* KEY_SUSPEND */
	case 247:   /* KEY_RFKILL */
	case 248:   /* KEY_MICMUTE */
	case 0x164: /* KEY_POWER2 */
	case 0x198: /* KEY_RESTART */
		return true;
	default:
		return false;
	}
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
		/* 键盘：全键码 + 自动重复，但系统键（电源/休眠/唤醒…）除外，
		 * 见 vdrm_key_system() 的注释。*/
		for (i = 0; i < VDRM_KEY_MAX; i++) {
			if (vdrm_key_system(i))
				continue;
			input_set_capability(d, VDRM_EV_KEY, i);
		}
		/* 双重保险：不管上面循环怎么写，能力表里都不许出现系统键。
		 *
		 * 为什么必须显式清：能力表里只要有 KEY_SYSRQ，input core 就会给
		 * 这台设备挂上 sysrq handler；只要有电源键，OnePlus 的
		 * oplus_keyevent_notifier 就会把它纳入电源键手势状态机
		 * （/proc/bus/input/devices 的 H: 行能直接看到这两个 handler）。
		 * 一条只给容器用的键盘不该有这些。*/
		for (i = 0; i < VDRM_KEY_MAX; i++) {
			if (vdrm_key_system(i))
				__clear_bit(i, d->keybit);
		}
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

	/* 广告给合成器的分辨率列表：基础两档 + （等 App 上报后）全屏横竖两档 */
	vdrm_modes_rebuild();
	pr_info("vdrm: advertised modes = %d\n", g_mode_count);

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

	rc = vdrm_audio_init(&g_pdev->dev);
	if (rc)
		pr_err("vdrm: audio init rc=%d (audio disabled)\n", rc);

	/* 池不在这里建，见上面"池不由内核建" */
	pr_info("vdrm: bring-up ok (dev=%px)\n", g_dev);
	return 0;
err:
	pr_err("vdrm: bring-up failed rc=%d\n", rc);
	/* 半成品按相反顺序拆掉：不回收的话 platform_device / drm_dev /
	 * misc / input / 声卡都会留在内核里，下次 insmod 撞资源。*/
	vdrm_audio_exit();
	vdrm_input_teardown();
	vdrm_do_unregister();
	return rc;
}

static void vdrm_exit(void)
{
	vdrm_audio_exit();
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
