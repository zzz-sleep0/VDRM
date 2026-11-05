#define _GNU_SOURCE
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <xf86drm.h>
#include <xf86drmMode.h>
int main(void){int fd=open("/dev/dri/card1",O_RDWR|O_CLOEXEC);if(fd<0){perror("open");return 1;}
 drmModeRes*r=drmModeGetResources(fd);if(!r){perror("res");return 1;}
 for(int i=0;i<r->count_crtcs;i++){drmModeCrtc*c=drmModeGetCrtc(fd,r->crtcs[i]);if(!c)continue;
  printf("crtc %u: fb=%u mode=%ux%u active=%d\n",c->crtc_id,c->buffer_id,c->mode.hdisplay,c->mode.vdisplay,c->mode_valid);drmModeFreeCrtc(c);}return 0;}
