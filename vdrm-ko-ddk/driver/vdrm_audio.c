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
static u32 g_last_tail;			/* pointer 上次看到的 App tail（字节）*/
static u64 g_consumed_bytes;		/* App 实际取走的字节数（自 prepare 起）*/
static void *g_mem;
static struct vdrm_audio_ring *g_ring;
static wait_queue_head_t g_wq;
static DEFINE_SPINLOCK(g_lock);
static int g_users;
static bool g_audio_up;
/* 串行化 kick 与模块卸载：exit 在锁内置空 g_pcm，kick 就不会碰到已释放的 PCM */
static DEFINE_MUTEX(g_kick_lock);

/* 虚拟卡没有真实 DMA 中断，标准栈（PipeWire）需要一个周期时钟才会启动
 * 它的图。RUNNING 期间用 hrtimer 按 period_size/rate 触发 period_elapsed。 */
static struct hrtimer g_period_timer;
static struct snd_pcm_substream *g_sub;
static u64 g_period_ns = 10000000ULL;
static bool g_period_on;

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
	used = used > g_ring->size ? g_ring->size : used;
	g_ring_used = used;
	return used;
}

/* App 每消费一段就调 VDRM_AUDIO_KICK：这里在流锁外推一次 hw_ptr，
 * 叫醒睡在 runtime->tsleep 上的阻塞写者（它默认只等 10 秒就 -EIO）。
 * 不能在 ack 里做，ack 持着流锁，period_elapsed 会重入。 */
static void vdrm_audio_kick(void)
{
	struct snd_pcm_substream *sub;

	mutex_lock(&g_kick_lock);
	if (g_pcm) {
		sub = g_pcm->streams[SNDRV_PCM_STREAM_PLAYBACK].substream;
		if (sub)
			snd_pcm_period_elapsed(sub);
	}
	mutex_unlock(&g_kick_lock);
}

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
	/* 从"现在"开始放：丢掉没人消费时环里可能残留的旧数据，免得一开 App
	 * 先放几秒陈音。head 追平 tail 就是"环空"。 */
	if (g_ring) {
		tail = smp_load_acquire(&g_ring->tail);
		smp_store_release(&g_ring->head, tail);
		g_last_tail = tail;
	}
	spin_unlock(&g_lock);
	return 0;
}

static int vdrm_audio_release(struct inode *inode, struct file *file)
{
	g_users = 0;
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
	info.magic = VDRM_AUDIO_MAGIC;
	info.size = g_ring->size;
	info.rate = g_ring->rate;
	info.channels = g_ring->channels;
	info.format = g_ring->format;
	info.header_size = sizeof(struct vdrm_audio_ring);

	if (copy_to_user((void __user *)arg, &info, sizeof(info)))
		return -EFAULT;
	return 0;
}

static int vdrm_audio_mmap(struct file *file, struct vm_area_struct *vma)
{
	/* mmap 的长度会被内核按页向上取整（32 + 512K → 129 页），所以这里
	 * 也必须按页对齐，否则合法映射会被下面的 size > total 拒掉。 */
	unsigned long total = PAGE_ALIGN(sizeof(struct vdrm_audio_ring) + g_ring->size);
	unsigned long size = vma->vm_end - vma->vm_start;

	if (size > total)
		return -EINVAL;
	if (remap_pfn_range(vma, vma->vm_start,
			    virt_to_phys(g_mem) >> PAGE_SHIFT,
			    size, vma->vm_page_prot))
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
			    SNDRV_PCM_INFO_MMAP_VALID,
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
	g_consumed_bytes = 0;
	g_ring_used = 0;
	wake_up_interruptible(&g_wq);
}

static int vdrm_pcm_close(struct snd_pcm_substream *sub)
{
	vdrm_period_stop();
	vdrm_ring_flush();
	g_sub = NULL;
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
	g_consumed_bytes = 0;
	return 0;
}

static int vdrm_pcm_trigger(struct snd_pcm_substream *sub, int cmd)
{
	switch (cmd) {
	case SNDRV_PCM_TRIGGER_START:
	case SNDRV_PCM_TRIGGER_RESUME:
	case SNDRV_PCM_TRIGGER_PAUSE_RELEASE:
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
	snd_pcm_uframes_t consumed;
	u32 tail, delta;

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
	return consumed % rt->buffer_size;
}

/* 低延迟：共享环里最多压 96KB（48k 立体声约 0.5 秒）。
 * 超过就从新数据里丢，避免 PulseAudio 启动时把环灌到 150KB+ 后一直不散。 */
#define VDRM_MAX_QUEUE_BYTES (96 * 1024)

/* 把一段 PCM 塞进共享环。绝不睡眠：ack/pointer 都在 ALSA 流锁里，
 * 环满就丢尾巴并记账，宁可咔一下也不能把内核线程挂住。 */
static void vdrm_ring_push(const u8 *src, u32 len)
{
	u32 size = g_ring->size;
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

	if (!frames || !rt->dma_area)
		return false;

	/* 这里必须用【未取模】的差：一次 ack 正好前进一个 buffer 是合法的
	 * （prepare 后写满整块），取模会得到 0，后面的 ack 就永远丢数据。 */
	delta = appl_ptr - g_last_appl;
	if (!delta)
		return false;
	if (delta > frames)		/* 防御，正常不会发生 */
		delta = frames;

	last = g_last_appl % frames;
	bytes = (u32)delta * fb;

	if (!g_users) {
		/* 没人在收：直接丢，别把容器里的播放器堵在写调用里 */
		g_discarded += bytes;
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
	return true;
}

/* ack 持流锁，不能在这里推 period；唤醒由 App 的 VDRM_AUDIO_KICK 负责。 */
static int vdrm_pcm_ack(struct snd_pcm_substream *sub)
{
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
	.mmap         = snd_pcm_lib_default_mmap,
};

/* ---------------- init / exit ---------------- */

int vdrm_audio_init(struct device *parent)
{
	unsigned long total = PAGE_ALIGN(sizeof(struct vdrm_audio_ring) + VDRM_AUDIO_RING_SIZE);
	int rc, i;

	hrtimer_init(&g_period_timer, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
	g_period_timer.function = vdrm_period_timer_fn;

	g_mem = alloc_pages_exact(total, GFP_KERNEL | __GFP_ZERO);
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
			snd_ctl_free_one(ctl);
			goto err_card;
		}
	}

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

	pr_info("vdrm: audio ready (/dev/vdrm-audio, %u Hz %u ch, ring %u)\n",
		g_ring->rate, g_ring->channels, g_ring->size);
	g_audio_up = true;
	return 0;

err_card:
	snd_card_free(g_card);
	g_card = NULL;
	g_pcm = NULL;
err_mem:
	free_pages_exact(g_mem, total);
	g_mem = NULL;
	g_ring = NULL;
	return rc;
}

void vdrm_audio_exit(void)
{
	unsigned long total = PAGE_ALIGN(sizeof(struct vdrm_audio_ring) + VDRM_AUDIO_RING_SIZE);

	if (g_audio_up) {
		misc_deregister(&g_audio_misc);
		g_audio_up = false;
	}
	/* 先摘掉 g_pcm：之后进来的 kick 直接返回；正在跑的 kick 由这把锁等完 */
	mutex_lock(&g_kick_lock);
	g_pcm = NULL;
	mutex_unlock(&g_kick_lock);
	g_period_on = false;
	hrtimer_cancel(&g_period_timer);
	g_sub = NULL;
	if (g_card) {
		snd_card_free(g_card);
		g_card = NULL;
	}
	if (g_mem) {
		free_pages_exact(g_mem, total);
		g_mem = NULL;
		g_ring = NULL;
	}
}
