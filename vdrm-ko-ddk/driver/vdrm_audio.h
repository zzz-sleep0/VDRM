/* vdrm_audio.h — vdrm.ko 音频环的共享 ABI（内核 + App 共用） */
#ifndef VDRM_AUDIO_H
#define VDRM_AUDIO_H

#include <linux/types.h>
#include <linux/ioctl.h>

#define VDRM_AUDIO_MAGIC      0x56445241u   /* "VDRA" */
#define VDRM_AUDIO_FMT_S16_LE 1
#define VDRM_AUDIO_RING_SIZE  (512 * 1024)
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

struct vdrm_audio_ring {
	__u32 magic;
	__u32 size;       /* data bytes */
	__u32 head;       /* kernel write offset, bytes */
	__u32 tail;       /* app read offset, bytes */
	__u32 rate;
	__u32 channels;
	__u32 format;
	__u32 reserved;
	__u8  data[];
};

#define VDRM_AUDIO_GET_INFO _IOR('v', 0x30, struct vdrm_audio_info)

#endif
