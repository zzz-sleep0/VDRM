/* vdrm_audio.h — vdrm.ko 音频环的共享 ABI（内核 + App 共用） */
#ifndef VDRM_AUDIO_H
#define VDRM_AUDIO_H

#include <linux/types.h>
#include <linux/ioctl.h>

#define VDRM_AUDIO_MAGIC      0x56445241u   /* "VDRA" */
#define VDRM_AUDIO_FMT_S16_LE 1
#define VDRM_AUDIO_RING_SIZE  (512 * 1024)
/* 采集环：ALSA 的 capture buffer 直接就是它（buffer_size == 16384 帧），
 * 所以大小必须和 hw 约束里的 buffer_bytes 一致。0.34 秒 @48k。*/
#define VDRM_AUDIO_IN_RING_SIZE (64 * 1024)
#define VDRM_AUDIO_RATE       48000
#define VDRM_AUDIO_CHANNELS   2

struct vdrm_audio_info {
	__u32 magic;
	__u32 size;
	__u32 rate;
	__u32 channels;
	__u32 format;
	__u32 header_size;
};

struct vdrm_audio_state {
	__u32 play_users;   /* container playback PCM open count */
	__u32 cap_users;    /* container capture PCM open count */
	__u32 flags;
	__u32 reserved;
};

struct vdrm_audio_ring {
	__u32 magic;
	__u32 size;       /* data bytes */
	__u32 head;       /* kernel write offset, bytes */
	__u32 tail;       /* app read offset, bytes */
	__u32 rate;
	__u32 channels;
	__u32 format;
	__u32 generation; /* prepare 时 +1，App 用它丢弃旧 tail */
	__u8  data[];
};

#define VDRM_AUDIO_GET_INFO _IOR('v', 0x30, struct vdrm_audio_info)
/* App 每消费一段（更新 tail）后调用：让内核推一次 hw_ptr 并叫醒 ALSA 写者。 */
#define VDRM_AUDIO_KICK     _IO('v', 0x31)
/* App 的 eventfd 注册到 /dev/vdrm-audio；容器打开/关闭 PCM 时内核 signal。
 * 失败（老模块）时 App 回退到轮询 GET_STATE。 */
#define VDRM_AUDIO_SET_EVENTFD _IOW('v', 0x32, int)
/* 容器侧 playback/capture PCM 当前的 open 计数。 */
#define VDRM_AUDIO_GET_STATE _IOR('v', 0x33, struct vdrm_audio_state)

#endif
