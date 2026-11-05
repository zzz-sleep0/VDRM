#define _GNU_SOURCE
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <xf86drm.h>

int main(void)
{
	int fd1 = open("/dev/dri/card1", O_RDWR | O_CLOEXEC);
	int fd2 = open("/dev/dri/renderD129", O_RDWR | O_CLOEXEC);
	drmDevicePtr a = NULL, b = NULL;

	if (fd1 < 0 || fd2 < 0) { perror("open"); return 2; }
	if (drmGetDevice2(fd1, 0, &a) || drmGetDevice2(fd2, 0, &b)) {
		printf("drmGetDevice2 failed\n");
		return 3;
	}
	printf("card1:  nodes=0x%x bustype=%d\n", a->available_nodes, a->bustype);
	printf("render: nodes=0x%x bustype=%d\n", b->available_nodes, b->bustype);
	printf("drmDevicesEqual = %d\n", drmDevicesEqual(a, b));

	drmDevicePtr devs[8];
	int n = drmGetDevices2(0, devs, 8);
	printf("drmGetDevices2 -> %d\n", n);
	for (int i = 0; i < n; i++)
		printf("  dev[%d] nodes=0x%x primary=%s render=%s\n", i,
		       devs[i]->available_nodes,
		       (devs[i]->available_nodes & (1 << DRM_NODE_PRIMARY)) ?
			       devs[i]->nodes[DRM_NODE_PRIMARY] : "-",
		       (devs[i]->available_nodes & (1 << DRM_NODE_RENDER)) ?
			       devs[i]->nodes[DRM_NODE_RENDER] : "-");
	return 0;
}
