/*
 * mkpool.c — 用户态"造池"程序（模块的 service.sh 在 boot 时拉起，跑完就退）
 *
 * 1) 用 gralloc 分配 N 块模式尺寸 AHB（真货，显示管线认）
 * 2) 每块取出 {fd0(像素), fd1(blob), ints} → REGISTER 进池
 *    （池由内核持住那两个 fd，所以本进程退出后池依旧在）
 *
 * 编译（bionic）：见同目录 Makefile —— aarch64-linux-android-clang，且不加 -O2
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

/* 分配档位：必须与 App 侧（anland 借 buffer 那条路）的分配质量对齐。
 *
 * 格式仍用 BGRA_8888：合成器写的是 DRM_FORMAT_XRGB8888，小端内存布局是
 * B,G,R,X。换成 RGBA 会被按 R,G,B,A 读，R/B 反转 —— 这里对齐的是物理
 * 布局语义，不是照抄 App 的 HAL 格式枚举。
 *
 * GPU_COLOR_OUTPUT 是关键位（对应 gralloc 的 HW_RENDER）：gralloc 据此
 * 决定是否按 GPU render target 选布局（UBWC / tile / 压缩）。之前缺这一位，
 * 池里的 buffer 很可能落成线性布局，和 App 自己分配的那批不是同一种东西。
 *
 * CPU_WRITE_OFTEN 暂时保留，和 App 侧的分配档位一致；它是否会把布局拉回
 * 线性，留到下一步 A/B（C 方案）再单独验证，避免一次改两个变量。
 *
 * 本机实测（1920x1080）：加上 GPU_COLOR_OUTPUT 后，native_handle 的
 * gralloc flags 里 0x80（PRIV_FLAGS_USES_UBWC）置位；再去掉 CPU_WRITE
 * 这个位也不变。说明真正决定布局档位的是 GPU_COLOR_OUTPUT。
 */
#define VDRM_AHB_FORMAT  5   /* HAL_PIXEL_FORMAT_BGRA_8888；NDK 没有对应 AHB 枚举 */
#define VDRM_AHB_USAGE   (AHARDWAREBUFFER_USAGE_CPU_WRITE_OFTEN | \
                          AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE | \
                          AHARDWAREBUFFER_USAGE_GPU_COLOR_OUTPUT | \
                          AHARDWAREBUFFER_USAGE_COMPOSER_OVERLAY)

#define NPOOL 6

int main(void)
{
    /*
     * 日志写在【自己旁边】—— 也就是模块目录里，跟着模块走（模块被卸掉日志一起没）。
     * 位置用 /proc/self/exe 反查，所以不把模块名/id 写死在这儿：模块改名、换 id
     * 都不用重编本程序。
     */
    char logpath[512];
    char *slash = NULL;
    ssize_t n = readlink("/proc/self/exe", logpath, sizeof(logpath) - 16);

    if (n > 0) {
        logpath[n] = '\0';
        slash = strrchr(logpath, '/');
    }
    if (slash)
        strcpy(slash + 1, "mkpool.log");
    else
        strcpy(logpath, "/data/local/tmp/vdrm-mkpool.log");

    FILE *lg = fopen(logpath, "w");
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
    P("alloc profile: fmt=%u usage=0x%llx\n",
      VDRM_AHB_FORMAT, (unsigned long long)VDRM_AHB_USAGE);

    /* 先清空旧池：避免多次跑 mkpool 在池里累积旧批次
     * （否则合成器会用到新批，而 App 只重建前 6 个 → 对不上）*/
    {
        struct vdrm_handle_req clr;
        memset(&clr, 0, sizeof(clr));
        if (ioctl(fd, VDRM_IOCTL_UNREGISTER, &clr) == 0)
            P("已清空旧池\n");
    }

    for (i = 0; i < NPOOL; i++) {
        AHardwareBuffer_Desc d, g;
        AHardwareBuffer *b = NULL;
        const native_handle_t *nh;
        struct vdrm_register_req req;
        uint32_t ni, k;

        memset(&d, 0, sizeof(d));
        d.width = w; d.height = h; d.layers = 1;
        d.format = VDRM_AHB_FORMAT;
        d.usage  = VDRM_AHB_USAGE;
        if (AHardwareBuffer_allocate(&d, &b) != 0 || !b) {
            P("AHB[%d] allocate failed\n", i); break;
        }
        AHardwareBuffer_describe(b, &g);
        P("AHB[%d] describe: %ux%u fmt=%u usage=0x%llx stride=%u layers=%u\n",
          i, g.width, g.height, g.format,
          (unsigned long long)g.usage, g.stride, g.layers);
        nh = AHardwareBuffer_getNativeHandle(b);
        if (!nh || nh->numFds != 2 || nh->numInts > MAX_INTS) {
            P("AHB[%d] layout unexpected numFds=%d numInts=%d\n",
              i, nh ? nh->numFds : -1, nh ? nh->numInts : -1);
            AHardwareBuffer_release(b);
            continue;
        }
        P("AHB[%d] handle: numFds=%d numInts=%d ints=",
          i, nh->numFds, nh->numInts);
        for (k = 0; k < (uint32_t)nh->numInts; k++)
            P("%s%08x", k ? "," : "", nh->data[2 + k]);
        P("\n");
        memset(&req, 0, sizeof(req));
        req.fd = nh->data[0];
        req.fd1 = nh->data[1];
        req.width  = g.width;
        req.height = g.height;
        req.pitch  = (uint32_t)g.stride * 4;   /* 真实 stride，不是 KPM 的 mode pitch */
        req.format = g.format;                 /* 真实 HAL 格式（5=BGRA），不是 DRM fourcc */
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
