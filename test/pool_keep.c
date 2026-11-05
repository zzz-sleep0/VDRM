/* pool_keep.c — 往 vdrm 池里注册 N 块 dma-heap 缓冲并常驻（模拟 mkpool，
 * 但用 dma-heap 而不是 gralloc），供 niri 这类合成器 CREATE_DUMB 使用。 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <stdlib.h>

#define VDRM_IOC(dir, type, nr, size) \
	((unsigned int)(((unsigned int)(dir) << 30) | ((unsigned int)(size) << 16) | \
			((unsigned int)(type) << 8) | (unsigned int)(nr)))
struct vdrm_info { uint32_t abi_version, width, height, format, pitch, pool_capacity, flags, reserved; };
struct vdrm_register_req {
	int32_t fd, fd1;
	uint32_t width, height, format, pitch, num_ints, id;
	uint32_t ints[40];
	uint64_t user_data;
};
struct vdrm_handle_req { uint32_t id, flags; };
struct dma_heap_allocation_data { uint64_t len; uint32_t fd, fd_flags; uint64_t heap_flags; };
#define VDRM_IOCTL_GET_INFO  VDRM_IOC(2, 'V', 0x00, sizeof(struct vdrm_info))
#define VDRM_IOCTL_REGISTER  VDRM_IOC(3, 'V', 0x01, sizeof(struct vdrm_register_req))
#define VDRM_IOCTL_UNREGISTER VDRM_IOC(1, 'V', 0x03, sizeof(struct vdrm_handle_req))
#define DMA_HEAP_IOCTL_ALLOC _IOWR('H', 0x0, struct dma_heap_allocation_data)

int main(int argc, char **argv)
{
	int n = argc > 1 ? atoi(argv[1]) : 6;
	int vfd = open("/dev/vdrm", O_RDWR | O_CLOEXEC);
	int hfd = open("/dev/dma_heap/system", O_RDWR | O_CLOEXEC);
	struct vdrm_info info;
	struct vdrm_handle_req clr = { 0, 0 };
	int i, ok = 0;

	if (vfd < 0 || hfd < 0) { perror("open"); return 1; }
	if (ioctl(vfd, VDRM_IOCTL_GET_INFO, &info)) { perror("GET_INFO"); return 1; }
	printf("mode %ux%u\n", info.width, info.height);
	ioctl(vfd, VDRM_IOCTL_UNREGISTER, &clr);
	for (i = 0; i < n; i++) {
		struct dma_heap_allocation_data al = { 0 };
		struct vdrm_register_req reg = { 0 };
		al.len = (uint64_t)info.width * info.height * 4;
		al.fd_flags = O_CLOEXEC;
		if (ioctl(hfd, DMA_HEAP_IOCTL_ALLOC, &al)) { perror("ALLOC"); break; }
		reg.fd = al.fd;
		reg.fd1 = -1;
		reg.width = info.width;
		reg.height = info.height;
		reg.format = 5;
		reg.pitch = info.width * 4;
		if (ioctl(vfd, VDRM_IOCTL_REGISTER, &reg)) { perror("REGISTER"); break; }
		printf("registered id=%u fd=%d\n", reg.id, al.fd);
		ok++;
	}
	fflush(stdout);
	printf("pool: %d/%d registered; keeping fds open\n", ok, n);
	fflush(stdout);
	sleep(1800);
	return 0;
}
