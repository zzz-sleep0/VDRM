#define _GNU_SOURCE
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <xf86drm.h>
#include <xf86drmMode.h>
int main(void)
{
	int fd = open("/dev/dri/card1", O_RDWR | O_CLOEXEC);
	drmModeRes *res;
	if (fd < 0) { perror("open"); return 1; }
	res = drmModeGetResources(fd);
	if (!res) { perror("res"); return 1; }
	for (int i = 0; i < res->count_connectors; i++) {
		drmModeConnector *c = drmModeGetConnector(fd, res->connectors[i]);
		if (!c) continue;
		printf("connector id=%u type=%u connection=%d (%s) modes=%d\n",
		       c->connector_id, c->connector_type, c->connection,
		       c->connection == DRM_MODE_CONNECTED ? "CONNECTED" :
		       c->connection == DRM_MODE_DISCONNECTED ? "DISCONNECTED" : "UNKNOWN",
		       c->count_modes);
		if (c->count_modes) printf("  mode0: %s %ux%u@%u\n", c->modes[0].name,
					   c->modes[0].hdisplay, c->modes[0].vdisplay,
					   c->modes[0].vrefresh);
		drmModeFreeConnector(c);
	}
	return 0;
}
