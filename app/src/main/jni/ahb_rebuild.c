/*
 * ahb_rebuild.c — 中继重建：把「原始 fd0(像素) + fd1(metadata blob) + ints」
 * 拼成 GraphicBuffer::flatten 线格式，经 SCM_RIGHTS 送一圈，拿回一个 AHardwareBuffer。
 *
 * 这是 Android 标准的 AHB 跨进程传递——**没有任何伪造/自校准/donor**：
 * 所有值都来自 gralloc 当初生成的那份 handle，所以 vendor gralloc 一定认。
 *
 * （原 anland 的 forge 方案已弃用：它给「非 gralloc 的 dma-buf」凭空造 handle，
 *   被 snapalloc 以 ENOSYS 拒绝；B/A′ 改走「KPM 保管 + 原样转发原生 fd」，
 *   于是根本不需要伪造。）
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <pthread.h>

#include <android/hardware_buffer.h>
#include <android/log.h>

#define TAG "VDRM"
#define FLOGI(...) __android_log_print(ANDROID_LOG_INFO,  TAG, __VA_ARGS__)
#define FLOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

#define MAX_INTS 40

/*
 * fd0        像素 dma-buf（KPM 转发的原始 file 的新 fd）
 * fd1        metadata blob（同上；-1 = 无）
 * ints       原始 handle 里的整数元数据（KPM 保管的）
 * num_ints   ints 的个数（<= MAX_INTS）
 * stride_bytes 真实行距（字节）
 * hal_fmt    原始 HAL 格式（如 1=R8G8B8A8_UNORM）
 */
AHardwareBuffer *vdrm_ahb_rebuild(int fd0, int fd1, const uint32_t *ints,
                                  uint32_t num_ints, uint32_t w, uint32_t h,
                                  uint32_t stride_bytes, uint32_t hal_fmt,
                                  uint64_t usage)
{
	int32_t head[13];
	int32_t *wire;
	size_t total;
	int sv[2], rc = -1, pix, blb;
	AHardwareBuffer *out = NULL;
	static pthread_mutex_t idlock = PTHREAD_MUTEX_INITIALIZER;
	static uint32_t counter = 0;
	uint64_t id;

	if (num_ints > MAX_INTS)
		num_ints = MAX_INTS;
	total = (size_t)(13 + num_ints) * 4;
	wire = (int32_t *)malloc(total);
	if (!wire)
		return NULL;

	/* 独立高位命名空间，避免与进程内真 GraphicBuffer 的 id 撞 */
	pthread_mutex_lock(&idlock);
	id = ((uint64_t)getpid() << 32) | (counter++ & 0xffffffffu) | (1ull << 63);
	pthread_mutex_unlock(&idlock);

	head[0]  = 0x47423031;                 /* 'GB01' */
	head[1]  = (int32_t)w;
	head[2]  = (int32_t)h;
	head[3]  = (int32_t)(stride_bytes / 4);
	head[4]  = (int32_t)hal_fmt;           /* 原始 HAL 格式 */
	head[5]  = 1;                          /* layerCount */
	head[6]  = (int32_t)usage;
	head[7]  = (int32_t)(id >> 32);
	head[8]  = (int32_t)id;
	head[9]  = 0;                          /* generationNumber */
	head[10] = 2;                          /* numFds: pixel + blob */
	head[11] = (int32_t)num_ints;
	head[12] = (int32_t)(usage >> 32);
	memcpy(wire, head, sizeof(head));
	memcpy(wire + 13, ints, (size_t)num_ints * 4);

	pix = dup(fd0);
	blb = (fd1 >= 0) ? dup(fd1) : -1;
	if (pix < 0 || blb < 0) {
		FLOGE("rebuild: dup failed errno=%d", errno);
		if (pix >= 0) close(pix);
		if (blb >= 0) close(blb);
		free(wire);
		return NULL;
	}
	if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0) {
		char cbuf[CMSG_SPACE(2 * sizeof(int))];
		struct iovec iov = { wire, total };
		struct msghdr msg;
		struct cmsghdr *cmsg;
		int fds[2];
		memset(&msg, 0, sizeof(msg));
		msg.msg_iov = &iov;
		msg.msg_iovlen = 1;
		msg.msg_control = cbuf;
		msg.msg_controllen = sizeof(cbuf);
		cmsg = CMSG_FIRSTHDR(&msg);
		cmsg->cmsg_level = SOL_SOCKET;
		cmsg->cmsg_type = SCM_RIGHTS;
		cmsg->cmsg_len = CMSG_LEN(2 * sizeof(int));
		fds[0] = pix; fds[1] = blb;
		memcpy(CMSG_DATA(cmsg), fds, sizeof(fds));
		msg.msg_controllen = cmsg->cmsg_len;
		if (sendmsg(sv[0], &msg, 0) >= 0)
			rc = AHardwareBuffer_recvHandleFromUnixSocket(sv[1], &out);
		else
			FLOGE("rebuild sendmsg: %s", strerror(errno));
		close(sv[0]);
		close(sv[1]);
	} else {
		FLOGE("rebuild socketpair: %s", strerror(errno));
	}
	free(wire);
	close(pix);
	close(blb);

	if (rc != 0 || !out) {
		FLOGE("rebuild refused rc=%d (%ux%u stride=%u ints=%u)",
		      rc, w, h, stride_bytes, num_ints);
		if (out) AHardwareBuffer_release(out);
		return NULL;
	}
	FLOGI("rebuild ok: %ux%u stride=%u -> %p", w, h, stride_bytes, (void *)out);
	return out;
}
