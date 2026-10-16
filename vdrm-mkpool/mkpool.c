/*
 * mkpool.c — 用户态"造池"程序（由 vdrm.sh up 拉起，跑完就退）
 *
 * 1) 用 gralloc 分配 N 块模式尺寸 AHB（真货，显示管线认）
 * 2) 每块取出 {fd0(像素), fd1(blob), ints} → REGISTER 进 KPM
 *    （KPM 会 fget 住这两个 fd 并持住池，所以本进程退出后池依旧在）
 *
 * 编译（bionic）：aarch64-linux-android-clang ... -landroid -lnativewindow -llog
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdint.h>
#include <sys/ioctl.h>
#include <android/hardware_buffer.h>

#ifndef MAX_INTS
#define MAX_INTS 40
#endif

/* NDK 没有 native_handle_t 头（VNDK 符号），和 App 一样本地声明 */
typedef struct native_handle {
    int version;
    int numFds;
    int numInts;
    int data[0];
} native_handle_t;
extern const native_handle_t *AHardwareBuffer_getNativeHandle(const AHardwareBuffer *b);

struct vdrm_info {
    uint32_t abi_version;
    uint32_t width, height, format, pitch;
    uint32_t pool_capacity;
    uint32_t flags;
    uint32_t reserved;
};
struct vdrm_handle_req {
    uint32_t id;      /* 0 = 清空整池 */
    uint32_t flags;
};
struct vdrm_register_req {
    int32_t  fd;      /* fd0 */
    int32_t  fd1;     /* fd1 */
    uint32_t width, height, format, pitch;
    uint32_t num_ints;
    uint32_t id;
    uint32_t ints[MAX_INTS];
    uint64_t user_data;
};
#define VDRM_IOC(dir, type, nr, size) \
    ((unsigned int)(((unsigned int)(dir) << 30) | \
                    ((unsigned int)(size) << 16) | \
                    ((unsigned int)(type) << 8) | (unsigned int)(nr)))
#define VDRM_IOCTL_GET_INFO  VDRM_IOC(2, 'V', 0x00, sizeof(struct vdrm_info))
#define VDRM_IOCTL_REGISTER  VDRM_IOC(3, 'V', 0x01, sizeof(struct vdrm_register_req))
#define VDRM_IOCTL_UNREGISTER VDRM_IOC(1, 'V', 0x03, sizeof(struct vdrm_handle_req))

#define NPOOL 6

int main(void)
{
    FILE *lg = fopen("/data/wayland/mkpool.log", "w");
    int fd, i, ok = 0;
    struct vdrm_info info;
    uint32_t w, h, fmt, pitch;

#define P(...) do { if (lg) { fprintf(lg, __VA_ARGS__); fflush(lg); } } while (0)

    fd = open("/dev/vdrm", O_RDWR | O_CLOEXEC);
    if (fd < 0) { P("open /dev/vdrm: %s\n", strerror(errno)); return 1; }

    memset(&info, 0, sizeof(info));
    if (ioctl(fd, VDRM_IOCTL_GET_INFO, &info) < 0) {
        P("GET_INFO: %s\n", strerror(errno)); return 1;
    }
    w = info.width; h = info.height; fmt = info.format; pitch = info.pitch;
    P("KPM mode %ux%u pitch=%u fmt=%08x\n", w, h, pitch, fmt);

    /* 先清空旧池：避免多次跑 mkpool 在池里累积旧批次
     * （否则合成器会用到新批，而 App 只重建前 6 个 → 对不上）*/
    {
        struct vdrm_handle_req clr;
        memset(&clr, 0, sizeof(clr));
        if (ioctl(fd, VDRM_IOCTL_UNREGISTER, &clr) == 0)
            P("已清空旧池\n");
    }

    for (i = 0; i < NPOOL; i++) {
        AHardwareBuffer_Desc d;
        AHardwareBuffer *b = NULL;
        const native_handle_t *nh;
        struct vdrm_register_req req;
        uint32_t ni;

        memset(&d, 0, sizeof(d));
        d.width = w; d.height = h; d.layers = 1;
        /* 关键：必须和合成器 GBM/dumb 路径的 DRM_FORMAT_XRGB8888 对齐
         * （小端下内存布局是 B,G,R,X）→ HAL 5 = BGRA_8888。
         * 用 1(RGBA) 会被按 RGBA 读 → 蓝黄(R/B)反转。*/
        d.format = 5;   /* AHARDWAREBUFFER_FORMAT_BGRA_8888 */
        d.usage = AHARDWAREBUFFER_USAGE_CPU_WRITE_OFTEN |
                  AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE |
                  AHARDWAREBUFFER_USAGE_COMPOSER_OVERLAY;
        if (AHardwareBuffer_allocate(&d, &b) != 0 || !b) {
            P("AHB[%d] allocate failed\n", i); break;
        }
        nh = AHardwareBuffer_getNativeHandle(b);
        if (!nh || nh->numFds != 2 || nh->numInts > MAX_INTS) {
            P("AHB[%d] layout unexpected numFds=%d numInts=%d\n",
              i, nh ? nh->numFds : -1, nh ? nh->numInts : -1);
            AHardwareBuffer_release(b);
            continue;
        }
        memset(&req, 0, sizeof(req));
        req.fd = nh->data[0];
        req.fd1 = nh->data[1];
        {
            AHardwareBuffer_Desc g;
            AHardwareBuffer_describe(b, &g);
            req.width  = g.width;
            req.height = g.height;
            req.pitch  = (uint32_t)g.stride * 4;   /* 真实 stride，不是 KPM 的 mode pitch */
            req.format = g.format;                 /* 真实 HAL 格式（1=RGBA），不是 DRM fourcc */
        }
        ni = (uint32_t)nh->numInts;
        req.num_ints = ni;
        memcpy(req.ints, &nh->data[2], ni * 4);
        if (ioctl(fd, VDRM_IOCTL_REGISTER, &req) < 0) {
            P("AHB[%d] REGISTER: %s\n", i, strerror(errno));
            AHardwareBuffer_release(b);
            continue;
        }
        P("AHB[%d] registered id=%u fd0=%d fd1=%d ints=%u\n",
          i, req.id, req.fd, req.fd1, ni);
        /* 故意【不 release】：本进程退出时 AHB 对象会被系统回收，
         * 但底层 dma-buf 已被 KPM fget 住，池不受影响。*/
        ok++;
    }
    P("done: %d/%d registered; 退出后池由 KPM 持住\n", ok, NPOOL);
    close(fd);
    if (lg) fclose(lg);
    return ok > 0 ? 0 : 2;
}
