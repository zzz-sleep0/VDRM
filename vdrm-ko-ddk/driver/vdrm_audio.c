/*
 * vdrm_audio.c — vdrm.ko 的虚拟声卡 + 共享音频环
 *
 * 数据路径（D 方案）：
 *   容器 PulseAudio/PipeWire -> ALSA playback (vdrm 卡)
 *     -> vdrm_pcm_copy 写进 512KB 环形缓冲
 *       -> App mmap /dev/vdrm-audio 读环 -> AAudio -> 手机喇叭
 *
 * 环头/数据都在同一块 alloc_pages_exact 内存里，App 通过 /dev/vdrm-audio
 * mmap 拿到；head 由内核写，tail 由 App 写，两个位置用 acquire/release 同步。
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
#include <sound/core.h>
#include <sound/pcm.h>
#include <sound/pcm_params.h>

#include "vdrm_audio.h"

static struct snd_card *g_card;
static struct snd_pcm *g_pcm;
static void *g_mem;
static struct vdrm_audio_ring *g_ring;
static wait_queue_head_t g_wq;
static DEFINE_SPINLOCK(g_lock);
static int g_users;

static u32 vdrm_audio_avail(void)
{
	u32 head = smp_load_acquire(&g_ring->head);
	u32 tail = smp_load_acquire(&g_ring->tail);
	return head - tail;
}

static u32 vdrm_audio_free_bytes(void)
{
	return g_ring->size - vdrm_audio_avail();
}

/* ---------------- char device ---------------- */

static int vdrm_audio_open(struct inode *inode, struct file *file)
{
	spin_lock(&g_lock);
	if (g_users) {
		spin_unlock(&g_lock);
		return -EBUSY;
	}
	g_users = 1;
	spin_unlock(&g_lock);
	return 0;
}

static int vdrm_audio_release(struct inode *inode, struct file *file)
{
	g_users = 0;
	wake_up_interruptible(&g_wq);
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
	unsigned long total = sizeof(struct vdrm_audio_ring) + g_ring->size;
	unsigned long size = vma->vm_end - vma->vm_start;

	if (size > total)
		return -EINVAL;
	if (remap_pfn_range(vma, vma->vm_start,
			    virt_to_phys(g_mem) >> PAGE_SHIFT,
			    size, vma->vm_page_prot))
		return -EAGAIN;
	vma->vm_flags |= VM_DONTEXPAND | VM_DONTDUMP;
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
	.info             = SNDRV_PCM_INFO_INTERLEAVED | SNDRV_PCM_INFO_BLOCK_TRANSFER,
	.formats          = SNDRV_PCM_FMTBIT_S16_LE,
	.rates            = SNDRV_PCM_RATE_48000,
	.rate_min         = VDRM_AUDIO_RATE,
	.rate_max         = VDRM_AUDIO_RATE,
	.channels_min     = VDRM_AUDIO_CHANNELS,
	.channels_max     = VDRM_AUDIO_CHANNELS,
	.buffer_bytes_max = VDRM_AUDIO_RING_SIZE,
	.period_bytes_min = 1024,
	.period_bytes_max = 64 * 1024,
	.periods_min      = 2,
	.periods_max      = 16,
};

static int vdrm_pcm_open(struct snd_pcm_substream *sub)
{
	struct snd_pcm_runtime *rt = sub->runtime;

	rt->hw = vdrm_pcm_hw;
	return snd_pcm_hw_constraint_integer(rt, SNDRV_PCM_HW_PARAM_PERIODS);
}

static int vdrm_pcm_close(struct snd_pcm_substream *sub)
{
	return 0;
}

static int vdrm_pcm_hw_params(struct snd_pcm_substream *sub,
			      struct snd_pcm_hw_params *params)
{
	return snd_pcm_lib_malloc_pages(sub, params_buffer_bytes(params));
}

static int vdrm_pcm_hw_free(struct snd_pcm_substream *sub)
{
	return snd_pcm_lib_free_pages(sub);
}

static int vdrm_pcm_prepare(struct snd_pcm_substream *sub)
{
	if (g_ring) {
		smp_store_release(&g_ring->head, 0);
		smp_store_release(&g_ring->tail, 0);
	}
	return 0;
}

static int vdrm_pcm_trigger(struct snd_pcm_substream *sub, int cmd)
{
	switch (cmd) {
	case SNDRV_PCM_TRIGGER_START:
	case SNDRV_PCM_TRIGGER_RESUME:
	case SNDRV_PCM_TRIGGER_PAUSE_RELEASE:
		wake_up_interruptible(&g_wq);
		return 0;
	case SNDRV_PCM_TRIGGER_STOP:
	case SNDRV_PCM_TRIGGER_SUSPEND:
	case SNDRV_PCM_TRIGGER_PAUSE_PUSH:
		return 0;
	}
	return -EINVAL;
}

static snd_pcm_uframes_t vdrm_pcm_pointer(struct snd_pcm_substream *sub)
{
	struct snd_pcm_runtime *rt = sub->runtime;
	u32 head = smp_load_acquire(&g_ring->head);
	u32 frames = head / (VDRM_AUDIO_CHANNELS * 2);

	if (rt->buffer_size)
		frames %= rt->buffer_size;
	return frames;
}

static int vdrm_pcm_copy(struct snd_pcm_substream *sub, int channel,
			 unsigned long pos, void __user *src, unsigned long count)
{
	u32 size = g_ring->size;

	while (count) {
		u32 head = smp_load_acquire(&g_ring->head);
		u32 tail = smp_load_acquire(&g_ring->tail);
		u32 free = size - (head - tail);
		u32 off, n, n1;

		if (!free) {
			if (wait_event_interruptible(g_wq, vdrm_audio_free_bytes() > 0))
				return -ERESTARTSYS;
			continue;
		}
		n = min_t(u32, count, free);
		off = head % size;
		n1 = min_t(u32, n, size - off);
		if (copy_from_user(g_ring->data + off, src, n1))
			return -EFAULT;
		if (n > n1 &&
		    copy_from_user(g_ring->data, (u8 __user *)src + n1, n - n1))
			return -EFAULT;
		smp_store_release(&g_ring->head, head + n);
		src = (u8 __user *)src + n;
		count -= n;
		snd_pcm_period_elapsed(sub);
		wake_up_interruptible(&g_wq);
	}
	return 0;
}

static int vdrm_pcm_fill_silence(struct snd_pcm_substream *sub, int channel,
				 unsigned long pos, unsigned long count)
{
	u32 size = g_ring->size;

	while (count) {
		u32 head = smp_load_acquire(&g_ring->head);
		u32 tail = smp_load_acquire(&g_ring->tail);
		u32 free = size - (head - tail);
		u32 off, n, n1;

		if (!free) {
			if (wait_event_interruptible(g_wq, vdrm_audio_free_bytes() > 0))
				return -ERESTARTSYS;
			continue;
		}
		n = min_t(u32, count, free);
		off = head % size;
		n1 = min_t(u32, n, size - off);
		memset(g_ring->data + off, 0, n1);
		if (n > n1)
			memset(g_ring->data, 0, n - n1);
		smp_store_release(&g_ring->head, head + n);
		count -= n;
		snd_pcm_period_elapsed(sub);
		wake_up_interruptible(&g_wq);
	}
	return 0;
}

static const struct snd_pcm_ops vdrm_pcm_ops = {
	.open         = vdrm_pcm_open,
	.close        = vdrm_pcm_close,
	.hw_params    = vdrm_pcm_hw_params,
	.hw_free      = vdrm_pcm_hw_free,
	.prepare      = vdrm_pcm_prepare,
	.trigger      = vdrm_pcm_trigger,
	.pointer      = vdrm_pcm_pointer,
	.copy         = vdrm_pcm_copy,
	.fill_silence = vdrm_pcm_fill_silence,
};

/* ---------------- init / exit ---------------- */

int vdrm_audio_init(struct device *parent)
{
	unsigned long total = sizeof(struct vdrm_audio_ring) + VDRM_AUDIO_RING_SIZE;
	int rc;

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
	if (rc)
		goto err_mem;
	strscpy(g_card->driver, "vdrm", sizeof(g_card->driver));
	strscpy(g_card->shortname, "vdrm", sizeof(g_card->shortname));
	strscpy(g_card->longname, "VDRM virtual audio", sizeof(g_card->longname));

	rc = snd_pcm_new(g_card, "vdrm", 0, 1, 0, &g_pcm);
	if (rc)
		goto err_card;
	snd_pcm_set_ops(g_pcm, SNDRV_PCM_STREAM_PLAYBACK, &vdrm_pcm_ops);
	snd_pcm_lib_preallocate_pages_for_all(g_pcm, SNDRV_DMA_TYPE_DEV,
					      parent, 64 * 1024, 256 * 1024);

	rc = snd_card_register(g_card);
	if (rc)
		goto err_card;

	rc = misc_register(&g_audio_misc);
	if (rc)
		goto err_card;

	pr_info("vdrm: audio ready (/dev/vdrm-audio, %u Hz %u ch, ring %u)\n",
		g_ring->rate, g_ring->channels, g_ring->size);
	return 0;

err_card:
	snd_card_free(g_card);
	g_card = NULL;
err_mem:
	free_pages_exact(g_mem, total);
	g_mem = NULL;
	g_ring = NULL;
	return rc;
}

void vdrm_audio_exit(void)
{
	unsigned long total = sizeof(struct vdrm_audio_ring) + VDRM_AUDIO_RING_SIZE;

	misc_deregister(&g_audio_misc);
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
