/*
 * vdrm_audio.c — vdrm.ko 的虚拟声卡 + 共享音频环
 *
 * 数据路径（D 方案）：
 *   容器 PulseAudio/PipeWire -> ALSA playback (vdrm 卡)
 *     -> vdrm_pcm_ack 把新帧从 ALSA 的 dma_area 搬进 512KB 环形缓冲
 *       -> App mmap /dev/vdrm-audio 读环 -> AAudio -> 手机喇叭
 *
 * 环头/数据都在同一块 alloc_pages_exact 内存里，App 通过 /dev/vdrm-audio
 * mmap 拿到；head 由内核写，tail 由 App 写，两个位置用 acquire/release 同步。
 *
 * 两个约定，改代码时别破坏：
 *   1. ack/pointer 都在 ALSA 流锁里跑，不能睡、不能调 snd_pcm_period_elapsed()
 *      （那把锁会重入）。环满就丢帧，绝不在内核里等 App。
 *   2. pointer 必须回报 App 的真实消费进度（tail），这样 ALSA 才有背压、
 *      容器里的播放器才会按实时速率出帧；App 没开时退化成"立即播完"，
 *      否则播放器会永远卡在写满 buffer 的等待里。
 *
 * 还有一条容易漏的：ALSA 的阻塞写在没空间时睡在 runtime->tsleep 上，
 * 默认只等 10 秒，超时直接 -EIO；它不会自己轮询 hw_ptr。所以 App 每消费
 * 一段就必须 ioctl(VDRM_AUDIO_KICK) 进来，由这里在锁外调
 * snd_pcm_period_elapsed() 推 hw_ptr、叫醒写者。App 关闭时 release 也会踢
 * 一次，让写者看到"没人收、直接丢"的退化路径。
 */
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/mm.h>
#include <linux/vmalloc.h>
#include <linux/miscdevice.h>
#include <linux/fs.h>
#include <linux/poll.h>
#include <linux/wait.h>
#include <linux/uaccess.h>
#include <linux/string.h>
#include <linux/version.h>
#include <linux/mutex.h>
#include <linux/hrtimer.h>
#include <sound/core.h>
#include <sound/control.h>
#include <sound/jack.h>
#include <sound/tlv.h>
#include <sound/pcm.h>
#include <sound/pcm_params.h>

#include "vdrm_audio.h"

static struct snd_card *g_card;
static struct snd_pcm *g_pcm;
static struct snd_jack *g_jack;
static snd_pcm_uframes_t g_last_appl;
static snd_pcm_uframes_t g_last_hw;	/* hw_ptr 绝对帧数，只许前进 */
static u32 g_last_tail;			/* pointer 上次看到的 App tail（字节）*/
static u64 g_consumed_bytes;		/* App 实际取走的字节数（自 prepare 起）*/
static void *g_mem;
static struct vdrm_audio_ring *g_ring;
static wait_queue_head_t g_wq;
static DEFINE_SPINLOCK(g_lock);
static int g_users;
static bool g_audio_up;
static bool g_audio_inited;		/* hrtimer 已初始化，exit 可以 cancel */
/* open() 不能直接改 head（那是 push 的单写者字段，两边锁不同）。改成置位，
 * 由持有 ALSA 流锁的 ack/pointer 在下次进入时补做"head 追平 tail"。*/
static atomic_t g_resync = ATOMIC_INIT(0);
/* 串行化 kick 与模块卸载：exit 在锁内置空 g_pcm，kick 就不会碰到已释放的 PCM */
static DEFINE_MUTEX(g_kick_lock);

/* 虚拟卡没有真实 DMA 中断，标准栈（PipeWire）需要一个周期时钟才会启动
 * 它的图。RUNNING 期间用 hrtimer 按 period_size/rate 触发 period_elapsed。 */
static struct hrtimer g_period_timer;
static struct snd_pcm_substream *g_sub;
static u64 g_period_ns = 10000000ULL;
static bool g_period_on;
/* hw_ptr 的时间基准：START 时的位置 + 单调时钟原点 */
static u64 g_hw_base;
static u64 g_hw_t0;

static enum hrtimer_restart vdrm_period_timer_fn(struct hrtimer *t)
{
	struct snd_pcm_substream *sub = g_sub;

	if (sub)
		snd_pcm_period_elapsed(sub);
	if (!g_period_on)
		return HRTIMER_NORESTART;
	hrtimer_forward_now(t, ns_to_ktime(g_period_ns));
	return HRTIMER_RESTART;
}

static void vdrm_period_start(struct snd_pcm_substream *sub)
{
	struct snd_pcm_runtime *rt = sub->runtime;
	u64 ns = 10000000ULL;		/* 10ms 兜底 */

	if (rt->rate && rt->period_size)
		ns = (u64)rt->period_size * 1000000000ULL / rt->rate;
	if (ns < 1000000ULL)
		ns = 1000000ULL;
	g_period_ns = ns;
	g_period_on = true;
	hrtimer_start(&g_period_timer, ns_to_ktime(ns), HRTIMER_MODE_REL);
}

static void vdrm_period_stop(void)
{
	g_period_on = false;
	hrtimer_try_to_cancel(&g_period_timer);
}

/* 诊断：这两个值只增不减，看 /sys/module/vdrm/parameters/ 下面对应的文件 */
static unsigned long long g_dropped;	/* 环满丢掉的字节（App 消费太慢）*/
static unsigned long long g_discarded;	/* App 没开时直接丢掉的字节 */
module_param_named(dropped_bytes, g_dropped, ullong, 0444);
MODULE_PARM_DESC(dropped_bytes, "Audio bytes dropped because the ring was full");
module_param_named(discarded_bytes, g_discarded, ullong, 0444);
MODULE_PARM_DESC(discarded_bytes, "Audio bytes discarded because no app consumed them");

/* 诊断：回调被调用的次数/字节数。虚拟声卡出问题时，"数据到底进没进环、
 * hw_ptr 有没有被推"是第一个要分清的事，光看 ring_used_bytes 分不清。*/
static unsigned long long g_st_ack_calls, g_st_ptr_calls;
static unsigned long long g_st_kick_calls, g_st_start_calls, g_st_early_delta;
static unsigned long long g_st_early_nodma, g_st_early_nousers, g_st_push_bytes;
static unsigned long long g_st_resync;
static unsigned int g_st_users;		/* App 打开 /dev/vdrm-audio 的次数 */
module_param_named(st_ack_calls, g_st_ack_calls, ullong, 0444);
MODULE_PARM_DESC(st_ack_calls, "Diagnostics: .ack callback invocations");
module_param_named(st_ptr_calls, g_st_ptr_calls, ullong, 0444);
MODULE_PARM_DESC(st_ptr_calls, "Diagnostics: .pointer callback invocations");
module_param_named(st_kick_calls, g_st_kick_calls, ullong, 0444);
MODULE_PARM_DESC(st_kick_calls, "Diagnostics: VDRM_AUDIO_KICK ioctls from the app");
module_param_named(st_start_calls, g_st_start_calls, ullong, 0444);
MODULE_PARM_DESC(st_start_calls, "Diagnostics: PCM trigger START/RESUME count");
module_param_named(st_early_delta, g_st_early_delta, ullong, 0444);
MODULE_PARM_DESC(st_early_delta, "Diagnostics: ack/pointer entered with appl_ptr unchanged");
module_param_named(st_early_nodma, g_st_early_nodma, ullong, 0444);
MODULE_PARM_DESC(st_early_nodma, "Diagnostics: ack/pointer entered without runtime dma_area");
module_param_named(st_early_nousers, g_st_early_nousers, ullong, 0444);
MODULE_PARM_DESC(st_early_nousers, "Diagnostics: bytes dropped because no app had the ring open");
module_param_named(st_push_bytes, g_st_push_bytes, ullong, 0444);
MODULE_PARM_DESC(st_push_bytes, "Diagnostics: bytes pushed into the shared ring");
module_param_named(st_resync, g_st_resync, ullong, 0444);
MODULE_PARM_DESC(st_resync, "Diagnostics: app-reattach ring resyncs");
module_param_named(st_users, g_st_users, uint, 0444);
MODULE_PARM_DESC(st_users, "Diagnostics: /dev/vdrm-audio open count");

/* 诊断：共享环里当前积压的字节数（约 192000 B/s）*/
static unsigned int g_ring_used;
module_param_named(ring_used_bytes, g_ring_used, uint, 0444);
MODULE_PARM_DESC(ring_used_bytes, "Audio bytes currently queued in the shared ring");

/* ALSA mixer：虚拟卡没有硬件 mixer，这里只提供控件让 ACP/WirePlumber
 * 能把卡初始化成可用 profile。音量不在这里衰减 —— 交给 PulseAudio/
 * PipeWire 的软件音量，否则 server 把控件设成 0 会把整条数据乘成静音。 */
static int g_volume = 100;		/* 0..100，仅暴露给 mixer */
static bool g_muted;

static u32 vdrm_audio_avail(void)
{
	u32 head = smp_load_acquire(&g_ring->head);
	u32 tail = smp_load_acquire(&g_ring->tail);
	u32 used = head - tail;

	/* tail 被 prepare 和 App 的 in-flight 写甩到 head 前面时，别把回绕值
	 * 当成"有 4GB 数据"。按满环处理，等 App 自己重同步。 */
	used = used > VDRM_AUDIO_RING_SIZE ? VDRM_AUDIO_RING_SIZE : used;
	g_ring_used = used;
	return used;
}

/* App 每消费一段就调 VDRM_AUDIO_KICK：这里在流锁外推一次 hw_ptr，
 * 叫醒睡在 runtime->tsleep 上的阻塞写者（它默认只等 10 秒就 -EIO）。
 * 不能在 ack 里做，ack 持着流锁，period_elapsed 会重入。 */
static void vdrm_audio_kick(void)
{
	struct snd_pcm_substream *sub;

	g_st_kick_calls++;
	mutex_lock(&g_kick_lock);
	if (g_pcm) {
		sub = g_pcm->streams[SNDRV_PCM_STREAM_PLAYBACK].substream;
		if (sub)
			snd_pcm_period_elapsed(sub);
	}
	mutex_unlock(&g_kick_lock);
}

/* ======================== capture：麦克风进容器 ========================
 *
 * 方向和播放相反：App 把 Android 侧录到的帧【写进】共享环，内核的 capture
 * PCM 从环里读给容器。
 *
 * 这里不假装 DMA：capture 没有"硬件写"，环本身就是数据源。做法是让
 * runtime->dma_area 直接指到环的数据区、ALSA buffer 大小 == 环大小，
 * ALSA core 从 dma_area 把数据 copy 给用户；hw_ptr = App 的写位置，
 * appl_ptr = 用户已经读到的位置。省掉一次内核拷贝。
 */
static struct snd_pcm *g_cap_pcm;
static struct snd_pcm_substream *g_cap_sub;
static struct vdrm_audio_ring *g_cap_ring;
static void *g_cap_mem;
static int g_cap_users;
static unsigned long long g_cap_overrun;	/* App 写太快、被丢掉的帧数 */
static u32 g_cap_fake_hw;			/* 无 App 时模拟的"采集进度" */
static u32 g_cap_appl_fb;			/* 最近一次的读进度（字节）*/
static u32 g_cap_last_head;			/* 上次看到的 App 写位置（字节）*/
static u64 g_cap_written;			/* App 累计写入的帧数（绝对）*/
module_param_named(cap_overrun, g_cap_overrun, ullong, 0444);
/* 诊断：App 写位置（环内字节偏移）、内核累计写入帧数、App 是否打开 */
static int vdrm_cap_head_get(char *buf, const struct kernel_param *kp)
{
	(void)kp;
	return scnprintf(buf, PAGE_SIZE, "%u\n",
			 g_cap_ring ? smp_load_acquire(&g_cap_ring->head) : 0);
}
static const struct kernel_param_ops vdrm_cap_head_ops = { .get = vdrm_cap_head_get };
module_param_cb(cap_head, &vdrm_cap_head_ops, NULL, 0444);

static int vdrm_cap_written_get(char *buf, const struct kernel_param *kp)
{
	(void)kp;
	return scnprintf(buf, PAGE_SIZE, "%llu\n", (unsigned long long)g_cap_written);
}
static const struct kernel_param_ops vdrm_cap_written_ops = { .get = vdrm_cap_written_get };
module_param_cb(cap_written, &vdrm_cap_written_ops, NULL, 0444);

static int vdrm_cap_users_get(char *buf, const struct kernel_param *kp)
{
	(void)kp;
	return scnprintf(buf, PAGE_SIZE, "%d\n", g_cap_users);
}
static const struct kernel_param_ops vdrm_cap_users_ops = { .get = vdrm_cap_users_get };
module_param_cb(cap_users, &vdrm_cap_users_ops, NULL, 0444);
MODULE_PARM_DESC(cap_overrun, "Capture frames dropped because the app wrote too far ahead");

static const struct snd_pcm_hardware vdrm_cap_hw = {
	/* 不给 MMAP：这个流走 RW，数据由 core 从 dma_area 拷给用户 */
	.info             = SNDRV_PCM_INFO_INTERLEAVED,
	.formats          = SNDRV_PCM_FMTBIT_S16_LE,
	.rates            = SNDRV_PCM_RATE_48000,
	.rate_min         = VDRM_AUDIO_RATE,
	.rate_max         = VDRM_AUDIO_RATE,
	.channels_min     = VDRM_AUDIO_CHANNELS,
	.channels_max     = VDRM_AUDIO_CHANNELS,
	/* buffer 必须恰好等于采集环：这样 dma_area 的偏移和环的偏移一致。
	 * 下限在 open() 里用 snd_pcm_hw_constraint_minmax 钉死
	 * （snd_pcm_hardware 只有 max 字段）。*/
	.buffer_bytes_max = VDRM_AUDIO_IN_RING_SIZE,
	.period_bytes_min = 1024,
	.period_bytes_max = 16384,
	.periods_min      = 4,
	.periods_max      = 16,
};

static int vdrm_cap_open(struct snd_pcm_substream *sub)
{
	struct snd_pcm_runtime *rt = sub->runtime;

	rt->hw = vdrm_cap_hw;
	/* 把 ALSA buffer 钉成和采集环一样大，hw_params 里才能直接
	 * dma_area = 环数据区，省掉一次拷贝。*/
	snd_pcm_hw_constraint_minmax(rt, SNDRV_PCM_HW_PARAM_BUFFER_BYTES,
				     VDRM_AUDIO_IN_RING_SIZE,
				     VDRM_AUDIO_IN_RING_SIZE);
	g_cap_sub = sub;
	return 0;
}

static int vdrm_cap_close(struct snd_pcm_substream *sub)
{
	if (g_cap_sub == sub)
		g_cap_sub = NULL;
	return 0;
}

static int vdrm_cap_hw_params(struct snd_pcm_substream *sub,
			      struct snd_pcm_hw_params *params)
{
	struct snd_pcm_runtime *rt = sub->runtime;
	u32 bytes = params_buffer_bytes(params);

	if (!g_cap_ring || bytes > VDRM_AUDIO_IN_RING_SIZE)
		return -EINVAL;
	rt->dma_area  = g_cap_ring->data;
	rt->dma_addr  = 0;
	rt->dma_bytes = bytes;
	return 0;
}

static int vdrm_cap_hw_free(struct snd_pcm_substream *sub)
{
	struct snd_pcm_runtime *rt = sub->runtime;

	rt->dma_area  = NULL;
	rt->dma_addr  = 0;
	rt->dma_bytes = 0;
	return 0;
}

static int vdrm_cap_prepare(struct snd_pcm_substream *sub)
{
	u32 head;

	if (!g_cap_ring)
		return -ENODEV;
	/* 丢掉排队中的旧数据：tail 追平 head，代次 +1 让 App 重同步。
	 * 环头是用户态可写的，这里把内核那份常量写回去。*/
	head = 0;
	/* 新一轮流：环位置归零。App 每轮都从共享环里读 head 再写，所以它会
	 * 跟着从 0 开始；否则内核的 appl 从 0 算、App 的 head 还停在上一轮的
	 * 绝对位置，hw - appl 一次就超过一个 buffer，ALSA 直接判 overrun。*/
	smp_store_release(&g_cap_ring->head, head);
	smp_store_release(&g_cap_ring->tail, head);
	WRITE_ONCE(g_cap_ring->magic, VDRM_AUDIO_MAGIC);
	WRITE_ONCE(g_cap_ring->size, VDRM_AUDIO_IN_RING_SIZE);
	WRITE_ONCE(g_cap_ring->rate, VDRM_AUDIO_RATE);
	WRITE_ONCE(g_cap_ring->channels, VDRM_AUDIO_CHANNELS);
	WRITE_ONCE(g_cap_ring->format, VDRM_AUDIO_FMT_S16_LE);
	smp_store_release(&g_cap_ring->generation,
			  smp_load_acquire(&g_cap_ring->generation) + 1);
	g_cap_fake_hw = 0;
	g_cap_last_head = 0;
	g_cap_written = 0;
	return 0;
}

static snd_pcm_uframes_t vdrm_cap_pointer(struct snd_pcm_substream *sub)
{
	struct snd_pcm_runtime *rt = sub->runtime;
	u32 fb = VDRM_AUDIO_CHANNELS * 2;
	snd_pcm_uframes_t hw;
	u32 appl, head;

	if (!rt->buffer_size || !g_cap_ring)
		return 0;

	appl = (u32)rt->control->appl_ptr;
	head = smp_load_acquire(&g_cap_ring->head);
	/* 读进度写回环里，App 据此算还剩多少空间 */
	/* tail 必须写成【环内偏移】。appl 是单调帧数，直接乘字节数会越过环大小，
	 * App 那边用 head-tail 算空间就会下溢成"环满"，从此不再写 -> 读者等在
	 * 空环上直到 ALSA 超时（-EIO "capture error (DMA or IRQ trouble?)"）。*/
	smp_store_release(&g_cap_ring->tail,
			  (appl * fb) % VDRM_AUDIO_IN_RING_SIZE);
	g_cap_appl_fb = appl * fb;

	/* 用"累计写入帧数"当 hw_ptr，而不是直接拿 head 换算：
	 * head 是环内偏移（会回绕、也会在 App 重开时被重置），直接返回会让
	 * hw_ptr 往回跳，ALSA 会当成回绕 + 巨大 delta 判 overrun（实测 -EPIPE）。
	 * 这里跟播放侧同一套做法：只把"新增的那段"累加进绝对位置。*/
	{
		u32 delta = (head - g_cap_last_head) % VDRM_AUDIO_IN_RING_SIZE;

		if (delta > VDRM_AUDIO_IN_RING_SIZE / 2) {
			/* 往回跳（App 重开/流重启）：不认，重新对齐 */
			g_cap_last_head = head;
		} else {
			g_cap_written += delta / fb;
			g_cap_last_head = head;
		}
	}

	if (!g_cap_users) {
		/* 没有 App 送麦克风数据：假装硬件在按节拍采静音，否则容器里的
		 * 录音会永远 block 在第一次 read()。
		 * 注意不能每次都返回 appl+period —— 那样 avail_update 多调几次
		 * 就会比 appl 超前一个 buffer，ALSA 立刻判 overrun（-EPIPE）。
		 * 只在 appl 追上来时才往前推一个 period。*/
		if (g_cap_fake_hw < appl + rt->period_size)
			g_cap_fake_hw = appl + rt->period_size;
		hw = g_cap_fake_hw;
	} else {
		hw = (snd_pcm_uframes_t)g_cap_written;
		if ((s32)(hw - appl) < 0) {
			/* 落后于读进度（App 刚重开）：对齐，别让核心看到负 delta */
			hw = appl;
			g_cap_written = appl;
		} else if ((u32)(hw - appl) > rt->buffer_size) {
			/* 超前一个 buffer 以上：丢掉超出的部分（计入 overrun），
			 * 把位置拉回到 appl+buffer 之后继续。*/
			g_cap_overrun += (u32)(hw - appl) - rt->buffer_size;
			hw = appl + rt->buffer_size;
			g_cap_written = hw;
		}
	}
	return hw % rt->buffer_size;
}

static int vdrm_cap_trigger(struct snd_pcm_substream *sub, int cmd)
{
	switch (cmd) {
	case SNDRV_PCM_TRIGGER_START:
	case SNDRV_PCM_TRIGGER_RESUME:
	case SNDRV_PCM_TRIGGER_PAUSE_RELEASE:
	case SNDRV_PCM_TRIGGER_STOP:
	case SNDRV_PCM_TRIGGER_SUSPEND:
	case SNDRV_PCM_TRIGGER_PAUSE_PUSH:
		return 0;
	}
	return -EINVAL;
}

static const struct snd_pcm_ops vdrm_cap_ops = {
	.open      = vdrm_cap_open,
	.close     = vdrm_cap_close,
	.hw_params = vdrm_cap_hw_params,
	.hw_free   = vdrm_cap_hw_free,
	.prepare   = vdrm_cap_prepare,
	.trigger   = vdrm_cap_trigger,
	.pointer   = vdrm_cap_pointer,
};

/* ---- 采集环的字符设备 /dev/vdrm-audio-in ----
 *
 * App 打开它 → mmap 到 header+data → 用 AAudio 录麦克风 → 写进 data →
 * ioctl(KICK) 让内核推一次 hw_ptr 把容器里的读者叫醒。
 */
static int vdrm_audio_in_open(struct inode *inode, struct file *file)
{
	unsigned long flags;

	spin_lock_irqsave(&g_lock, flags);
	if (g_cap_users) {
		spin_unlock_irqrestore(&g_lock, flags);
		return -EBUSY;
	}
	g_cap_users = 1;
	spin_unlock_irqrestore(&g_lock, flags);

	if (g_cap_ring) {
		WRITE_ONCE(g_cap_ring->magic, VDRM_AUDIO_MAGIC);
		WRITE_ONCE(g_cap_ring->size, VDRM_AUDIO_IN_RING_SIZE);
		WRITE_ONCE(g_cap_ring->rate, VDRM_AUDIO_RATE);
		WRITE_ONCE(g_cap_ring->channels, VDRM_AUDIO_CHANNELS);
		WRITE_ONCE(g_cap_ring->format, VDRM_AUDIO_FMT_S16_LE);
		/* head/tail 对齐到"当前读进度"，而不是 0：读流可能已经跑了
		 * 一段（静音期），App 一打开就把位置拨回 0 的话，hw_ptr 会
		 * 往回跳，ALSA 会当成回绕 -> 直接判 overrun（-EPIPE）。*/
		{
			u32 base = g_cap_appl_fb % VDRM_AUDIO_IN_RING_SIZE;

			smp_store_release(&g_cap_ring->head, base);
			smp_store_release(&g_cap_ring->tail, base);
			g_cap_last_head = base;
			g_cap_written   = g_cap_appl_fb /
					  (VDRM_AUDIO_CHANNELS * 2);
		}
		smp_store_release(&g_cap_ring->generation,
				  smp_load_acquire(&g_cap_ring->generation) + 1);
		/* 没有人送数据时容器读到的必须是静音 */
		memset(g_cap_ring->data, 0, VDRM_AUDIO_IN_RING_SIZE);
	}
	return 0;
}

static int vdrm_audio_in_release(struct inode *inode, struct file *file)
{
	unsigned long flags;

	spin_lock_irqsave(&g_lock, flags);
	g_cap_users = 0;
	spin_unlock_irqrestore(&g_lock, flags);
	if (g_cap_ring)
		memset(g_cap_ring->data, 0, VDRM_AUDIO_IN_RING_SIZE);
	/* 唤醒可能正等着数据的 capture 读者 */
	mutex_lock(&g_kick_lock);
	if (g_cap_sub)
		snd_pcm_period_elapsed(g_cap_sub);
	mutex_unlock(&g_kick_lock);
	return 0;
}

static long vdrm_audio_in_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	struct vdrm_audio_info info;

	if (cmd == VDRM_AUDIO_KICK) {
		mutex_lock(&g_kick_lock);
		if (g_cap_sub)
			snd_pcm_period_elapsed(g_cap_sub);
		mutex_unlock(&g_kick_lock);
		return 0;
	}
	if (cmd != VDRM_AUDIO_GET_INFO)
		return -ENOTTY;
	if (!arg)
		return -EINVAL;

	memset(&info, 0, sizeof(info));
	info.magic       = VDRM_AUDIO_MAGIC;
	info.size        = VDRM_AUDIO_IN_RING_SIZE;
	info.rate        = VDRM_AUDIO_RATE;
	info.channels    = VDRM_AUDIO_CHANNELS;
	info.format      = VDRM_AUDIO_FMT_S16_LE;
	info.header_size = sizeof(struct vdrm_audio_ring);
	if (copy_to_user((void __user *)arg, &info, sizeof(info)))
		return -EFAULT;
	return 0;
}

static int vdrm_audio_in_mmap(struct file *file, struct vm_area_struct *vma)
{
	unsigned long total = PAGE_ALIGN(sizeof(struct vdrm_audio_ring) +
					 VDRM_AUDIO_IN_RING_SIZE);
	unsigned long size = vma->vm_end - vma->vm_start;

	if (!g_cap_ring)
		return -ENODEV;
	if (size > total)
		return -EINVAL;
	if (remap_vmalloc_range(vma, g_cap_mem, 0))
		return -EAGAIN;
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 1, 0)
	vm_flags_set(vma, VM_DONTEXPAND | VM_DONTDUMP);
#else
	vma->vm_flags |= VM_DONTEXPAND | VM_DONTDUMP;
#endif
	return 0;
}

static const struct file_operations vdrm_audio_in_fops = {
	.owner          = THIS_MODULE,
	.open           = vdrm_audio_in_open,
	.release        = vdrm_audio_in_release,
	.unlocked_ioctl = vdrm_audio_in_ioctl,
	.mmap           = vdrm_audio_in_mmap,
	.llseek         = no_llseek,
};

static struct miscdevice g_audio_in_misc = {
	.minor = MISC_DYNAMIC_MINOR,
	.name  = "vdrm-audio-in",
	.fops  = &vdrm_audio_in_fops,
	.mode  = 0600,
};

/* ---------------- char device ---------------- */

static int vdrm_audio_open(struct inode *inode, struct file *file)
{
	u32 tail;

	spin_lock(&g_lock);
	if (g_users) {
		spin_unlock(&g_lock);
		return -EBUSY;
	}
	g_users = 1;
	g_st_users = 1;
	if (g_ring) {
		/* 环头是用户态可写的：每次打开都把内核那份常量字段写回去，
		 * 免得被改过的 size/rate/format 影响后续计算。*/
		WRITE_ONCE(g_ring->magic, VDRM_AUDIO_MAGIC);
		WRITE_ONCE(g_ring->size, VDRM_AUDIO_RING_SIZE);
		WRITE_ONCE(g_ring->rate, VDRM_AUDIO_RATE);
		WRITE_ONCE(g_ring->channels, VDRM_AUDIO_CHANNELS);
		WRITE_ONCE(g_ring->format, VDRM_AUDIO_FMT_S16_LE);
		tail = smp_load_acquire(&g_ring->tail);
		g_last_tail = tail;
		g_consumed_bytes = 0;
		g_last_hw = 0;
		/* "环空"的动作交给持有流锁的 ack/pointer 去做 */
		atomic_set(&g_resync, 1);
	}
	spin_unlock(&g_lock);
	return 0;
}

static int vdrm_audio_release(struct inode *inode, struct file *file)
{
	/* 裸写会盖掉并发的 open：旧进程 fput 的 release 晚于新进程的 open
	 * 时，驱动会永远以为"没人收"，之后一路丢帧。*/
	spin_lock(&g_lock);
	g_users = 0;
	g_st_users = 0;
	spin_unlock(&g_lock);
	wake_up_interruptible(&g_wq);
	/* 消费者没了：立刻推一次，让阻塞写者看到 pointer 的"直接丢"退化路径，
	 * 不用干等 10 秒。 */
	vdrm_audio_kick();
	return 0;
}

static __poll_t vdrm_audio_poll(struct file *file, poll_table *wait)
{
	poll_wait(file, &g_wq, wait);
	if (vdrm_audio_avail() > 0)
		return EPOLLIN | EPOLLRDNORM;
	return 0;
}

static long vdrm_audio_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	struct vdrm_audio_info info;

	if (cmd == VDRM_AUDIO_KICK) {
		vdrm_audio_kick();
		return 0;
	}
	if (cmd != VDRM_AUDIO_GET_INFO)
		return -ENOTTY;
	if (!arg)
		return -EINVAL;

	memset(&info, 0, sizeof(info));
	/* 全部用编译期常量：这些字段在共享页里，用户态可以改，不能拿来
	 * 告诉 App"你的环有多大"。*/
	info.magic = VDRM_AUDIO_MAGIC;
	info.size = VDRM_AUDIO_RING_SIZE;
	info.rate = VDRM_AUDIO_RATE;
	info.channels = VDRM_AUDIO_CHANNELS;
	info.format = VDRM_AUDIO_FMT_S16_LE;
	info.header_size = sizeof(struct vdrm_audio_ring);

	if (copy_to_user((void __user *)arg, &info, sizeof(info)))
		return -EFAULT;
	return 0;
}

static int vdrm_audio_mmap(struct file *file, struct vm_area_struct *vma)
{
	/* mmap 的长度会被内核按页向上取整（32 + 512K → 129 页），所以这里
	 * 也必须按页对齐，否则合法映射会被下面的 size > total 拒掉。 */
	/* 几何只用编译期常量：环头（含 size 字段）是用户态可写的，
	 * 绝不能拿它算内核侧的映射长度/拷贝边界。*/
	unsigned long total = PAGE_ALIGN(sizeof(struct vdrm_audio_ring) +
					 VDRM_AUDIO_RING_SIZE);
	unsigned long size = vma->vm_end - vma->vm_start;

	if (!g_ring)
		return -ENODEV;
	if (size > total)
		return -EINVAL;
	if (remap_vmalloc_range(vma, g_mem, 0))
		return -EAGAIN;
	/* 6.1 的 Android GKI 把 vm_flags 改成了 const（回移 6.3 的改动），
	 * 只能用 vm_flags_set()；5.10/5.15 还没有这个 helper。 */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 1, 0)
	vm_flags_set(vma, VM_DONTEXPAND | VM_DONTDUMP);
#else
	vma->vm_flags |= VM_DONTEXPAND | VM_DONTDUMP;
#endif
	return 0;
}

static const struct file_operations vdrm_audio_fops = {
	.owner          = THIS_MODULE,
	.open           = vdrm_audio_open,
	.release        = vdrm_audio_release,
	.poll           = vdrm_audio_poll,
	.unlocked_ioctl = vdrm_audio_ioctl,
	.compat_ioctl   = vdrm_audio_ioctl,
	.mmap           = vdrm_audio_mmap,
};

static struct miscdevice g_audio_misc = {
	.minor = MISC_DYNAMIC_MINOR,
	.name  = "vdrm-audio",
	.fops  = &vdrm_audio_fops,
	.mode  = 0666,
};

/* ---------------- ALSA ---------------- */

static const struct snd_pcm_hardware vdrm_pcm_hw = {
	/* 标准栈（PipeWire）会优先走 mmap；dma_area 由 snd_pcm_lib_malloc_pages
	 * 分配，snd_pcm_lib_default_mmap 可以直接映射。 */
	.info             = SNDRV_PCM_INFO_INTERLEAVED |
			    SNDRV_PCM_INFO_MMAP |
			    SNDRV_PCM_INFO_MMAP_VALID |
			    /* 不给 mmap 客户端"直写 appl_ptr"的免检通道：有了它，
			     * alsa-lib 走 SYNC_PTR，ack 才会被调，也才有屏障保证
			     * 我们拷到的样本已经被客户端写完。*/
			    SNDRV_PCM_INFO_SYNC_APPLPTR,
	.formats          = SNDRV_PCM_FMTBIT_S16_LE,
	.rates            = SNDRV_PCM_RATE_48000,
	.rate_min         = VDRM_AUDIO_RATE,
	.rate_max         = VDRM_AUDIO_RATE,
	.channels_min     = VDRM_AUDIO_CHANNELS,
	.channels_max     = VDRM_AUDIO_CHANNELS,
	/* ALSA buffer 只留 32KB（48k 立体声约 170ms），避免播放器把
	 * 512KB 的共享环当 ALSA buffer 填满，造成 1s+ 的音画延迟。 */
	.buffer_bytes_max = 32 * 1024,
	.period_bytes_min = 1024,
	.period_bytes_max = 8 * 1024,
	.periods_min      = 2,
	.periods_max      = 8,
};

static int vdrm_pcm_open(struct snd_pcm_substream *sub)
{
	struct snd_pcm_runtime *rt = sub->runtime;

	g_sub = sub;
	rt->hw = vdrm_pcm_hw;
	return snd_pcm_hw_constraint_integer(rt, SNDRV_PCM_HW_PARAM_PERIODS);
}

/* 停止/关闭时清空共享环：视频关掉后不要把已经排进环的 0.5 秒尾巴
 * 继续放完。抬 generation 让 App 同步丢弃本地 tail。 */
static void vdrm_ring_flush(void)
{
	u32 gen;

	if (!g_ring)
		return;
	gen = smp_load_acquire(&g_ring->generation) + 1;
	smp_store_release(&g_ring->head, 0);
	smp_store_release(&g_ring->tail, 0);
	smp_store_release(&g_ring->generation, gen);
	g_last_appl = 0;
	g_last_tail = 0;
	g_last_hw = 0;
	g_consumed_bytes = 0;
	g_ring_used = 0;
	wake_up_interruptible(&g_wq);
}

static int vdrm_pcm_close(struct snd_pcm_substream *sub)
{
	/* 这里在 snd_pcm_release 里、可睡眠；紧接着 ALSA core 就 kfree
	 * runtime。try_to_cancel 不等正在跑的回调，回调随后会踩到已释放的
	 * status/control —— 必须用会等待的 cancel。*/
	g_period_on = false;
	if (g_audio_inited)
		hrtimer_cancel(&g_period_timer);
	g_sub = NULL;
	vdrm_ring_flush();
	return 0;
}

static int vdrm_pcm_hw_params(struct snd_pcm_substream *sub,
			      struct snd_pcm_hw_params *params)
{
	u32 bytes = params_buffer_bytes(params);

	return snd_pcm_lib_malloc_pages(sub, bytes);
}

static int vdrm_pcm_hw_free(struct snd_pcm_substream *sub)
{
	vdrm_ring_flush();
	return snd_pcm_lib_free_pages(sub);
}

static int vdrm_pcm_prepare(struct snd_pcm_substream *sub)
{
	vdrm_period_stop();
	if (g_ring) {
		u32 gen = smp_load_acquire(&g_ring->generation) + 1;

		smp_store_release(&g_ring->head, 0);
		smp_store_release(&g_ring->tail, 0);
		/* 最后再抬代次：App 看到新代次时，head/tail 已经清干净。 */
		smp_store_release(&g_ring->generation, gen);
	}
	g_last_appl = 0;
	g_last_tail = 0;
	g_last_hw = 0;
	g_hw_base = 0;
	g_hw_t0 = ktime_get_ns();
	g_consumed_bytes = 0;
	return 0;
}

static int vdrm_pcm_trigger(struct snd_pcm_substream *sub, int cmd)
{
	switch (cmd) {
	case SNDRV_PCM_TRIGGER_START:
	case SNDRV_PCM_TRIGGER_RESUME:
	case SNDRV_PCM_TRIGGER_PAUSE_RELEASE:
		g_st_start_calls++;
		/* 时间基准从"此刻的 hw 位置"开始算 */
		g_hw_base = g_last_hw;
		g_hw_t0 = ktime_get_ns();
		vdrm_period_start(sub);
		wake_up_interruptible(&g_wq);
		return 0;
	case SNDRV_PCM_TRIGGER_STOP:
	case SNDRV_PCM_TRIGGER_SUSPEND:
		vdrm_period_stop();
		vdrm_ring_flush();
		return 0;
	case SNDRV_PCM_TRIGGER_PAUSE_PUSH:
		vdrm_period_stop();
		return 0;
	}
	return -EINVAL;
}

/* 回报 App 真正取走的帧数（tail），让 ALSA 有背压。
 *
 * 以前这里直接返回 appl_ptr，等于告诉 ALSA"写进来就算播完了"：播放器会一直
 * 满速灌满 512KB 环，然后被内核丢帧 —— 延迟越来越大、音画对不上。
 *
 * App 没开（g_users == 0）时退回 appl_ptr：没有消费者，若还按 tail 算，
 * 容器里的播放器写满 buffer 后会永远等下去。此时 ack 也会直接丢数据。
 */
static bool vdrm_pcm_ack_push(struct snd_pcm_substream *sub,
			      snd_pcm_uframes_t appl_ptr);

static snd_pcm_uframes_t vdrm_pcm_pointer(struct snd_pcm_substream *sub)
{
	struct snd_pcm_runtime *rt = sub->runtime;
	snd_pcm_uframes_t fb = VDRM_AUDIO_CHANNELS * 2;
	snd_pcm_uframes_t consumed, hw;
	u32 tail, delta;

	g_st_ptr_calls++;
	if (!rt->buffer_size || !g_ring)
		return 0;

	/* mmap 模式下客户端直接写 dma_area 并更新 appl_ptr，ack 不一定被调；
	 * 在 pointer 里收割新帧，RW 模式下这时 delta 通常已经是 0。 */
	vdrm_pcm_ack_push(sub, rt->control->appl_ptr);

	if (!g_users) {
		g_consumed_bytes = (u64)rt->control->appl_ptr * fb;
		g_last_tail = smp_load_acquire(&g_ring->tail);
		return rt->control->appl_ptr % rt->buffer_size;
	}

	tail = smp_load_acquire(&g_ring->tail);
	delta = tail - g_last_tail;	/* u32 回绕是良性的 */
	delta -= delta % fb;		/* 只认整帧 */
	if (delta) {
		g_consumed_bytes += delta;
		g_last_tail += delta;
	}

	consumed = g_consumed_bytes / fb;
	/* 防御：prepare 和 App 的 tail 更新撞上时，别让 hw_ptr 跑到 appl_ptr 前面 */
	if (consumed > rt->control->appl_ptr) {
		consumed = rt->control->appl_ptr;
		g_consumed_bytes = (u64)consumed * fb;
	}

	/* 关键：hw_ptr 必须按【时间】走，不能只按"App 取走了多少"走。
	 *
	 * App 消费是瞬时的（拿到就写进 AAudio），如果 hw_ptr 跟着它走，
	 * 播 512 帧（10ms）的数据会在几十微秒内变成"播完了"，
	 * avail == buffer_size >= stop_threshold，ALSA 立刻判 XRUN —— 实测
	 * 就是写完第一小段就 -EPIPE、之后所有 write 都 -EIO。
	 *
	 * 真声卡是两个约束同时成立：不会超过已写入的量（appl_ptr），
	 * 也不会超过真实时间的进度（这里按 rate 算）。
	 * App 跟不上时 consumed 自然小于时间进度，hw_ptr 就退化成"消费者进度"，
	 * 于是容器写满就挡回去 —— 这正是我们要的背压。*/
	hw = consumed;
	/* 5.15 起 snd_pcm_runtime.state 被挪进 status->state（6.1 两处都有）。
	 * 用 status 那份，跨版本都成立。*/
	if (rt->status && rt->status->state == SNDRV_PCM_STATE_RUNNING && rt->rate) {
		u64 ns = ktime_get_ns() - g_hw_t0;
		u64 timed = g_hw_base + div_u64(ns, 1000000000ULL) * rt->rate +
			    div_u64((ns % 1000000000ULL) * rt->rate, 1000000000ULL);

		if (timed < hw)
			hw = timed;
	}

	/* hw_ptr 只能前进：ALSA 把"新值比旧值小"当成回绕，并据此伪造一次
	 * 整 buffer 的前进（xrun）。rewind 之后必须钳住。*/
	if (hw < g_last_hw)
		hw = g_last_hw;
	else
		g_last_hw = hw;
	return hw % rt->buffer_size;
}

/* 低延迟：共享环里最多压 96KB（48k 立体声约 0.5 秒）。
 * 超过就从新数据里丢，避免 PulseAudio 启动时把环灌到 150KB+ 后一直不散。 */
#define VDRM_MAX_QUEUE_BYTES (96 * 1024)

/* 把一段 PCM 塞进共享环。绝不睡眠：ack/pointer 都在 ALSA 流锁里，
 * 环满就丢尾巴并记账，宁可咔一下也不能把内核线程挂住。 */
static void vdrm_ring_push(const u8 *src, u32 len)
{
	u32 size = VDRM_AUDIO_RING_SIZE;	/* 编译期常量，不看可写的环头 */
	u32 dropped = 0;

	while (len) {
		u32 head = smp_load_acquire(&g_ring->head);
		u32 tail = smp_load_acquire(&g_ring->tail);
		u32 used = head - tail;
		u32 free;
		u32 off, n, n1;

		if (used >= VDRM_MAX_QUEUE_BYTES) {
			dropped = len;
			break;
		}
		free = used >= size ? 0 : size - used;
		/* 96KB 上限在单次 push 里也要生效，不能只看循环开头的 used */
		if (used < VDRM_MAX_QUEUE_BYTES) {
			u32 room = VDRM_MAX_QUEUE_BYTES - used;

			if (free > room)
				free = room;
		} else {
			free = 0;
		}
		if (!free) {
			dropped = len;
			break;
		}
		n = min_t(u32, len, free);
		off = head % size;
		n1 = min_t(u32, n, size - off);
		memcpy(g_ring->data + off, src, n1);
		if (n > n1)
			memcpy(g_ring->data, src + n1, n - n1);
		smp_store_release(&g_ring->head, head + n);
		src += n;
		len -= n;
	}
	if (dropped) {
		g_dropped += dropped;
		pr_warn_ratelimited("vdrm: audio ring full, dropped %u bytes (total %llu)\n",
				    dropped, g_dropped);
	}
	g_ring_used = vdrm_audio_avail();
	wake_up_interruptible(&g_wq);
}

/* ALSA 把用户数据写进自己的 dma_area 后回调 ack；这里把新帧搬进 vdrm 环。
 * ack 是在 ALSA 流锁里被调的，所以不能睡、也不能调 snd_pcm_period_elapsed()
 * （会重入同一把锁）。唤醒写者的活交给 App 的 VDRM_AUDIO_KICK。
 * 返回 true 表示这一轮确实往环里推了数据。 */
static bool vdrm_pcm_ack_push(struct snd_pcm_substream *sub, snd_pcm_uframes_t appl_ptr)
{
	struct snd_pcm_runtime *rt = sub->runtime;
	snd_pcm_uframes_t frames = rt->buffer_size;
	snd_pcm_uframes_t delta, last, first;
	u32 fb = VDRM_AUDIO_CHANNELS * 2;
	u32 bytes;

	if (!frames || !rt->dma_area) {
		g_st_early_nodma++;
		return false;
	}

	/* App（重）打开时请求过"丢弃旧数据"：head 追平 tail 就等价于环空。
	 * 这里在 ALSA 流锁里做，是 head 的唯一写者。*/
	if (atomic_xchg(&g_resync, 0)) {
		u32 tail = smp_load_acquire(&g_ring->tail);
		/* 这一轮刚写进来的帧数：重同步只该丢掉环里那些"App 没取走"的
		 * 老数据，不能把这轮新帧一起吞掉。
		 * 老版本这里顺手写了 g_last_appl = appl_ptr，于是紧接着的
		 * delta 变成 0、这一轮一帧都推不下去 —— 实测表现就是：容器
		 * 写完 8192 帧（一个 hw buffer）以后 hw_ptr 一直不动，
		 * 10 秒后 -EIO，而环里 0 字节、App 那边也没有数据可消费。*/
		u32 pending = appl_ptr - g_last_appl;

		smp_store_release(&g_ring->head, tail);
		g_last_tail = tail;
		/* 丢弃的那些算已消费（它们不会再到 App 手里），
		 * 本轮 pending 帧照常推下去。*/
		g_consumed_bytes = (u64)(appl_ptr - pending) * fb;
		g_last_hw = appl_ptr - pending;
		g_st_resync++;
	}

	/* appl_ptr 回退（snd_pcm_rewind）：客户端把已经写过的帧"收回"了，
	 * 那些数据不能拷。无符号减法会回绕成巨大值，必须先按有符号判。*/
	if ((snd_pcm_sframes_t)(appl_ptr - g_last_appl) < 0) {
		g_last_appl = appl_ptr;
		return false;
	}

	/* 这里必须用【未取模】的差：一次 ack 正好前进一个 buffer 是合法的
	 * （prepare 后写满整块），取模会得到 0，后面的 ack 就永远丢数据。 */
	delta = appl_ptr - g_last_appl;
	if (!delta) {
		g_st_early_delta++;
		return false;
	}
	if (delta > frames)		/* 防御，正常不会发生 */
		delta = frames;

	last = g_last_appl % frames;
	bytes = (u32)delta * fb;

	if (!g_users) {
		/* 没人在收：直接丢，别把容器里的播放器堵在写调用里 */
		g_discarded += bytes;
		g_st_early_nousers += bytes;
		g_last_appl = appl_ptr;
		return false;
	}

	first = (frames - last) * fb;
	if (first > bytes)
		first = bytes;
	vdrm_ring_push((const u8 *)rt->dma_area + last * fb, (u32)first);
	if (bytes > first)
		vdrm_ring_push((const u8 *)rt->dma_area, bytes - (u32)first);

	g_last_appl = appl_ptr;
	g_st_push_bytes += bytes;
	return true;
}

/* ack 持流锁，不能在这里推 period；唤醒由 App 的 VDRM_AUDIO_KICK 负责。 */
static int vdrm_pcm_ack(struct snd_pcm_substream *sub)
{
	g_st_ack_calls++;
	vdrm_pcm_ack_push(sub, sub->runtime->control->appl_ptr);
	return 0;
}

/* ---------------- mixer ---------------- */

static int vdrm_vol_info(struct snd_kcontrol *kcontrol,
			 struct snd_ctl_elem_info *uinfo)
{
	uinfo->type = SNDRV_CTL_ELEM_TYPE_INTEGER;
	uinfo->count = 1;
	uinfo->value.integer.min = 0;
	uinfo->value.integer.max = 100;
	return 0;
}

static int vdrm_vol_get(struct snd_kcontrol *kcontrol,
			struct snd_ctl_elem_value *ucontrol)
{
	ucontrol->value.integer.value[0] = g_volume;
	return 0;
}

static int vdrm_vol_put(struct snd_kcontrol *kcontrol,
			struct snd_ctl_elem_value *ucontrol)
{
	int v = clamp((int)ucontrol->value.integer.value[0], 0, 100);

	if (v == g_volume)
		return 0;
	g_volume = v;
	return 1;
}

static int vdrm_switch_info(struct snd_kcontrol *kcontrol,
			    struct snd_ctl_elem_info *uinfo)
{
	return snd_ctl_boolean_mono_info(kcontrol, uinfo);
}

static int vdrm_switch_get(struct snd_kcontrol *kcontrol,
			   struct snd_ctl_elem_value *ucontrol)
{
	ucontrol->value.integer.value[0] = g_muted ? 0 : 1;
	return 0;
}

static int vdrm_switch_put(struct snd_kcontrol *kcontrol,
			   struct snd_ctl_elem_value *ucontrol)
{
	bool muted = !ucontrol->value.integer.value[0];

	if (muted == g_muted)
		return 0;
	g_muted = muted;
	return 1;
}

static const unsigned int vdrm_vol_tlv[] = {
	SNDRV_CTL_TLVD_DB_SCALE_ITEM(-10000, 100, 0),
};

static const struct snd_kcontrol_new vdrm_controls[] = {
	{
		.iface = SNDRV_CTL_ELEM_IFACE_MIXER,
		.name = "Master Playback Volume",
		.access = SNDRV_CTL_ELEM_ACCESS_READWRITE |
			  SNDRV_CTL_ELEM_ACCESS_TLV_READ,
		.info = vdrm_vol_info,
		.get = vdrm_vol_get,
		.put = vdrm_vol_put,
		.tlv = { .p = vdrm_vol_tlv },
	},
	{
		.iface = SNDRV_CTL_ELEM_IFACE_MIXER,
		.name = "PCM Playback Volume",
		.access = SNDRV_CTL_ELEM_ACCESS_READWRITE |
			  SNDRV_CTL_ELEM_ACCESS_TLV_READ,
		.info = vdrm_vol_info,
		.get = vdrm_vol_get,
		.put = vdrm_vol_put,
		.tlv = { .p = vdrm_vol_tlv },
	},
	{
		.iface = SNDRV_CTL_ELEM_IFACE_MIXER,
		.name = "Master Playback Switch",
		.access = SNDRV_CTL_ELEM_ACCESS_READWRITE,
		.info = vdrm_switch_info,
		.get = vdrm_switch_get,
		.put = vdrm_switch_put,
	},
};

static const struct snd_pcm_ops vdrm_pcm_ops = {
	.open         = vdrm_pcm_open,
	.close        = vdrm_pcm_close,
	.hw_params    = vdrm_pcm_hw_params,
	.hw_free      = vdrm_pcm_hw_free,
	.prepare      = vdrm_pcm_prepare,
	.trigger      = vdrm_pcm_trigger,
	.pointer      = vdrm_pcm_pointer,
	.ack          = vdrm_pcm_ack,
	/* 不设 .mmap：NULL 时 ALSA core 自己走 snd_pcm_lib_default_mmap()。
	 * 直接引用那个符号在部分 GKI 内核上会 Unknown symbol（实测本机
	 * 6.1.157 不导出它），而行为完全一样。*/
};

/* ---------------- init / exit ---------------- */

int vdrm_audio_init(struct device *parent)
{
	unsigned long total = PAGE_ALIGN(sizeof(struct vdrm_audio_ring) + VDRM_AUDIO_RING_SIZE);
	int rc, i;

	hrtimer_init(&g_period_timer, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
	g_period_timer.function = vdrm_period_timer_fn;
	/* 之后 vdrm_audio_exit() 可能被模块的失败回滚调用（init 从未成功），
	 * 所以用它区分"定时器初始化过没有"，别对零初始化的 hrtimer 做 cancel。*/
	g_audio_inited = true;

	/* 用 vmalloc_user 而不是 alloc_pages_exact：
	 * 528384 B 要 256 页物理连续（order-8），在跑久了的手机上经常拿不到
	 * ——实测本机 6.1.157 直接 -ENOMEM，声音整条消失且只有一行日志。
	 * vmalloc_user 出来的区域带 VM_USERMAP，mmap 用 remap_vmalloc_range()
	 * 逐页 vm_insert_page，也不再需要"物理连续"和 virt_to_phys。*/
	g_mem = vmalloc_user(total);
	if (!g_mem)
		return -ENOMEM;
	g_ring = g_mem;
	g_ring->magic = VDRM_AUDIO_MAGIC;
	g_ring->size = VDRM_AUDIO_RING_SIZE;
	g_ring->rate = VDRM_AUDIO_RATE;
	g_ring->channels = VDRM_AUDIO_CHANNELS;
	g_ring->format = VDRM_AUDIO_FMT_S16_LE;
	init_waitqueue_head(&g_wq);

	rc = snd_card_new(parent, -1, "vdrm", THIS_MODULE, 0, &g_card);
	if (rc) {
		pr_err("vdrm: audio snd_card_new rc=%d\n", rc);
		goto err_mem;
	}
	strscpy(g_card->driver, "vdrm", sizeof(g_card->driver));
	strscpy(g_card->shortname, "vdrm", sizeof(g_card->shortname));
	strscpy(g_card->longname, "VDRM virtual audio", sizeof(g_card->longname));

	/* 虚拟耳机口：始终“已插入”，让 ACP 有 Speaker/Headphone 端口可建。 */
	rc = snd_jack_new(g_card, "Speaker", SND_JACK_HEADPHONE, &g_jack,
			  true, true);
	if (rc) {
		pr_err("vdrm: audio snd_jack_new rc=%d\n", rc);
		goto err_card;
	}
	snd_jack_report(g_jack, SND_JACK_HEADPHONE);

	rc = snd_pcm_new(g_card, "vdrm", 0, 1, 0, &g_pcm);
	if (rc) {
		pr_err("vdrm: audio snd_pcm_new rc=%d\n", rc);
		goto err_card;
	}
	snd_pcm_set_ops(g_pcm, SNDRV_PCM_STREAM_PLAYBACK, &vdrm_pcm_ops);
	snd_pcm_lib_preallocate_pages_for_all(g_pcm, SNDRV_DMA_TYPE_DEV,
					      parent, 64 * 1024, 256 * 1024);

	for (i = 0; i < ARRAY_SIZE(vdrm_controls); i++) {
		struct snd_kcontrol *ctl = snd_ctl_new1(&vdrm_controls[i], NULL);

		if (!ctl) {
			rc = -ENOMEM;
			goto err_card;
		}
		rc = snd_ctl_add(g_card, ctl);
		if (rc) {
			/* 这里本该 snd_ctl_free_one(ctl) 回收，但本机内核没有导出
			 * 这个符号（Unknown symbol），引用它会让整个模块加载失败。
			 * 只在 ENOMEM 错误路径漏一个控件，可以接受。*/
			goto err_card;
		}
	}

	/* ---- 采集环 + capture PCM（麦克风进容器）---- */
	{
		unsigned long total_in = PAGE_ALIGN(sizeof(struct vdrm_audio_ring) +
						    VDRM_AUDIO_IN_RING_SIZE);

		g_cap_mem = vmalloc_user(total_in);
		if (!g_cap_mem) {
			rc = -ENOMEM;
			goto err_card;
		}
		g_cap_ring = g_cap_mem;
		memset(g_cap_mem, 0, total_in);
		g_cap_ring->magic    = VDRM_AUDIO_MAGIC;
		g_cap_ring->size     = VDRM_AUDIO_IN_RING_SIZE;
		g_cap_ring->rate     = VDRM_AUDIO_RATE;
		g_cap_ring->channels = VDRM_AUDIO_CHANNELS;
		g_cap_ring->format   = VDRM_AUDIO_FMT_S16_LE;
	}

	rc = snd_pcm_new(g_card, "vdrm-mic", 1, 0, 1, &g_cap_pcm);
	if (rc) {
		pr_err("vdrm: audio snd_pcm_new(capture) rc=%d\n", rc);
		goto err_cap_mem;
	}
	snd_pcm_set_ops(g_cap_pcm, SNDRV_PCM_STREAM_CAPTURE, &vdrm_cap_ops);

	rc = snd_card_register(g_card);
	if (rc) {
		pr_err("vdrm: audio snd_card_register rc=%d\n", rc);
		goto err_card;
	}

	rc = misc_register(&g_audio_misc);
	if (rc) {
		pr_err("vdrm: audio misc_register rc=%d\n", rc);
		goto err_card;
	}

	rc = misc_register(&g_audio_in_misc);
	if (rc) {
		pr_err("vdrm: audio in misc_register rc=%d\n", rc);
		misc_deregister(&g_audio_misc);
		goto err_card;
	}

	pr_info("vdrm: audio ready (/dev/vdrm-audio out %u Hz %u ch ring %u, "
		"/dev/vdrm-audio-in cap %u)\n",
		g_ring->rate, g_ring->channels, g_ring->size,
		VDRM_AUDIO_IN_RING_SIZE);
	g_audio_up = true;
	return 0;

err_cap_mem:
	if (g_cap_mem) {
		vfree(g_cap_mem);
		g_cap_mem = NULL;
		g_cap_ring = NULL;
	}

err_card:
	snd_card_free(g_card);
	g_card = NULL;
	g_pcm = NULL;
err_mem:
	vfree(g_mem);
	g_mem = NULL;
	g_ring = NULL;
	return rc;
}

void vdrm_audio_exit(void)
{
	unsigned long total = PAGE_ALIGN(sizeof(struct vdrm_audio_ring) + VDRM_AUDIO_RING_SIZE);

	if (g_audio_up) {
		misc_deregister(&g_audio_in_misc);
		misc_deregister(&g_audio_misc);
		g_audio_up = false;
	}
	/* 先摘掉 g_pcm：之后进来的 kick 直接返回；正在跑的 kick 由这把锁等完 */
	mutex_lock(&g_kick_lock);
	g_pcm = NULL;
	mutex_unlock(&g_kick_lock);
	g_period_on = false;
	if (g_audio_inited) {
		hrtimer_cancel(&g_period_timer);
		g_audio_inited = false;
	}
	g_sub = NULL;
	if (g_card) {
		snd_card_free(g_card);
		g_card = NULL;
	}
	if (g_mem) {
		vfree(g_mem);
		g_mem = NULL;
		g_ring = NULL;
	}
	if (g_cap_mem) {
		vfree(g_cap_mem);
		g_cap_mem = NULL;
		g_cap_ring = NULL;
	}
	g_cap_sub = NULL;
	g_cap_users = 0;
}
