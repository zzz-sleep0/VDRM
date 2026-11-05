#define _GNU_SOURCE
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <poll.h>
#include <sys/ioctl.h>
#define VDRM_IOC(dir, type, nr, size) \
	((unsigned int)(((unsigned int)(dir) << 30) | ((unsigned int)(size) << 16) | \
			((unsigned int)(type) << 8) | (unsigned int)(nr)))
struct vdrm_pool_info { int32_t fd, fd1; uint32_t id, width, height, pitch, format, num_ints; uint32_t ints[40]; uint32_t flags; };
struct vdrm_pool_list { uint32_t count, reserved; struct vdrm_pool_info items[16]; };
struct vdrm_frame_event { uint32_t id, sequence; uint64_t timestamp_ns, user_data; };
#define VDRM_IOCTL_GET_POOL VDRM_IOC(3, 'V', 0x07, sizeof(struct vdrm_pool_list))
int main(void)
{
	int fd = open("/dev/vdrm", O_RDWR | O_NONBLOCK | O_CLOEXEC);
	struct vdrm_pool_list l;
	struct pollfd p = { 0 };
	if (fd < 0) { perror("open"); return 1; }
	if (ioctl(fd, VDRM_IOCTL_GET_POOL, &l)) { perror("GET_POOL"); return 1; }
	printf("pool count=%u ids:", l.count);
	for (unsigned i = 0; i < l.count; i++) {
		printf(" %u", l.items[i].id);
		if (l.items[i].fd >= 0) close(l.items[i].fd);
		if (l.items[i].fd1 >= 0) close(l.items[i].fd1);
	}
	printf("\n");
	p.fd = fd; p.events = POLLIN;
	if (poll(&p, 1, 200) > 0) {
		struct vdrm_frame_event ev;
		if (read(fd, &ev, sizeof(ev)) == sizeof(ev))
			printf("frame event: id=%u seq=%u\n", ev.id, ev.sequence);
	} else {
		printf("no pending frame event (idle compositor)\n");
	}
	return 0;
}
