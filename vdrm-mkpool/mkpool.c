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
struct vdrm_screen_req {
    uint32_t width;
    uint32_t height;
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
#define VDRM_IOCTL_SET_SCREEN VDRM_IOC(1, 'V', 0x0b, sizeof(struct vdrm_screen_req))

/* 面板尺寸：模块用它推出"全面屏"的横竖两档分辨率（modes 里那两档 1080p 以外
 * 的尺寸就是这里来的）。
 *
 * 以前只有 App 会上报，而且是【窗口尺寸】—— 于是出现一个很难受的顺序依赖：
 * App 不起，modes 里就只有 1080x1920 / 1920x1080 两档，写 mode=1264x2780
 * 会被 rebuild 掉，合成器只能退回 16:9。窗口尺寸本来也不该决定"这块屏有多大"。
 *
 * 现在 mkpool 自己查：`wm size` 的输出里有 Physical size，设了 Override 就用
 * Override（那是用户/系统实际生效的分辨率）。查不到就不动，模块保留原值。
 * 传 --no-screen 可以跳过这一步。*/
/* 写一个 /sys/module/vdrm/parameters/ 下的参数。 */
static int write_param(const char *name, const char *val)
{
    char path[128];
    FILE *f;
    int rc;

    snprintf(path, sizeof(path), "/sys/module/vdrm/parameters/%s", name);
    f = fopen(path, "w");
    if (!f)
        return -1;
    rc = fputs(val, f);
    fputc('\n', f);
    fclose(f);
    return rc < 0 ? -1 : 0;
}

/* 目标尺寸：从 mode 参数读，而不是从 GET_INFO 读。
 *
 * GET_INFO 的 width/height 是 g_active，也就是"合成器此刻真正在用的尺寸"
 * （App 靠它决定转屏）。运行中切分辨率时，参数已经是新尺寸、合成器还没
 * modeset，这时候按 g_active 建池就会建到旧尺寸上。
 * 参数读不到才退回 GET_INFO。*/
static int query_mode_param(uint32_t *w, uint32_t *h)
{
    FILE *f = fopen("/sys/module/vdrm/parameters/mode", "r");
    unsigned int a, b;
    int ok = -1;

    if (!f)
        return -1;
    if (fscanf(f, "%ux%u", &a, &b) == 2 && a && b) {
        *w = a;
        *h = b;
        ok = 0;
    }
    fclose(f);
    return ok;
}

static int query_screen(uint32_t *w, uint32_t *h)
{
    FILE *fp = popen("wm size 2>/dev/null", "r");
    char line[128];
    uint32_t pw = 0, ph = 0, ow = 0, oh = 0;

    if (!fp)
        return -1;

    while (fgets(line, sizeof(line), fp)) {
        unsigned int a, b;

        if (sscanf(line, "Physical size: %ux%u", &a, &b) == 2) {
            pw = a; ph = b;
        } else if (sscanf(line, "Override size: %ux%u", &a, &b) == 2) {
            ow = a; oh = b;
        }
    }
    pclose(fp);

    if (ow && oh) { *w = ow; *h = oh; return 0; }
    if (pw && ph) { *w = pw; *h = ph; return 0; }
    return -1;
}

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

int main(int argc, char **argv)
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
    int fd, i, ok = 0, have = 0, need;
    struct vdrm_info info;
    uint32_t w, h, fmt, pitch;

#define P(...) do { if (lg) { fprintf(lg, __VA_ARGS__); fflush(lg); } } while (0)

    fd = open("/dev/vdrm", O_RDWR | O_CLOEXEC);
    if (fd < 0) { P("open /dev/vdrm: %s\n", strerror(errno)); return 1; }

    /* 先把面板尺寸告诉模块，modes 列表里才会有"这块屏"的横竖两档。
     * 必须在 GET_INFO / 建池之前 —— 建池看的是模块当前的生效尺寸。*/
    {
        int no_screen = 0;

        for (i = 1; i < argc; i++)
            if (strcmp(argv[i], "--no-screen") == 0)
                no_screen = 1;

        if (!no_screen) {
            uint32_t sw = 0, sh = 0;

            if (query_screen(&sw, &sh) == 0) {
                struct vdrm_screen_req sr;

                sr.width = sw;
                sr.height = sh;
                if (ioctl(fd, VDRM_IOCTL_SET_SCREEN, &sr) == 0)
                    P("screen %ux%u -> KPM\n", sw, sh);
                else
                    P("SET_SCREEN: %s\n", strerror(errno));
            } else {
                P("wm size 查不到，沿用 KPM 现有尺寸\n");
            }
        }
    }

    memset(&info, 0, sizeof(info));
    if (ioctl(fd, VDRM_IOCTL_GET_INFO, &info) < 0) {
        P("GET_INFO: %s\n", strerror(errno)); return 1;
    }
    w = info.width; h = info.height; fmt = info.format; pitch = info.pitch;
    P("KPM active %ux%u pitch=%u fmt=%08x\n", w, h, pitch, fmt);

    /* --mode WxH：一次完整的运行时切换。
     *   1) 写 mode 参数 = prepare（只记目标，模块不发 hotplug）
     *   2) 按目标尺寸把槽补齐
     *   3) 池齐了再写 commit = 更新 EDID + hotplug
     * 顺序固定，所以合成器收到热插拔时槽一定已经在了。*/
    const char *mode_arg = NULL;
    const char *pool_arg = NULL;

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--mode") == 0 && i + 1 < argc)
            mode_arg = argv[++i];
        else if (strcmp(argv[i], "--pool") == 0 && i + 1 < argc)
            pool_arg = argv[++i];
    }

    /* --pool WxH：只给这个尺寸补槽，**不写 mode 参数、不 commit**。
     *
     * 内核在合成器要一个"广告过但还没槽"的尺寸时，会通过 want_mode 参数 +
     * eventfd 叫 App；App 就跑这一条。这样分辨率完全归合成器挑，我们只负责
     * 把槽准备好。*/
    if (pool_arg) {
        uint32_t pw = 0, ph = 0;

        if (sscanf(pool_arg, "%ux%u", &pw, &ph) == 2 && pw && ph) {
            w = pw;
            h = ph;
            P("只补槽：%ux%u（不改 mode、不 commit）\n", w, h);
        } else {
            P("--pool 参数格式不对（要 WxH）：%s\n", pool_arg);
        }
    }

    if (pool_arg) {
        /* 尺寸已在上面按 --pool 设好；不能再走 "取 mode 参数" 的兜底，
         * 否则会被当前生效尺寸覆盖掉。*/
    } else if (mode_arg) {
        uint32_t mw = 0, mh = 0;

        if (sscanf(mode_arg, "%ux%u", &mw, &mh) == 2 && mw && mh) {
            if (write_param("mode", mode_arg) == 0)
                P("prepare: mode -> %ux%u\n", mw, mh);
            else
                P("写 mode 参数失败: %s\n", strerror(errno));
            w = mw;
            h = mh;
        } else {
            P("--mode 参数格式不对（要 WxH）：%s\n", mode_arg);
        }
    } else {
        uint32_t mw = 0, mh = 0;

        if (query_mode_param(&mw, &mh) == 0 && (mw != w || mh != h)) {
            P("目标尺寸取 mode 参数：%ux%u（合成器当前 %ux%u）\n", mw, mh, w, h);
            w = mw;
            h = mh;
        }
    }
    P("alloc profile: fmt=%u usage=0x%llx\n",
      VDRM_AHB_FORMAT, (unsigned long long)VDRM_AHB_USAGE);

    /* 保留 / 补齐：只在显式 --reset 时清池。
     *
     * 为什么要改：切分辨率时清整池会让"旧尺寸的槽"一起消失，切换要等一整轮
     * 重新分配（1–2 秒黑），而且来回切尺寸等于反复全清全建。现在改成"看看这档
     * 还缺几个，只补缺的"，其它尺寸的槽原样留着 —— 来回切就几乎零成本。*/
    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--reset") == 0) {
            struct vdrm_handle_req clr;

            memset(&clr, 0, sizeof(clr));
            if (ioctl(fd, VDRM_IOCTL_UNREGISTER, &clr) == 0)
                P("--reset: 已清空旧池\n");
            break;
        }
    }

    {
        FILE *f = fopen("/sys/module/vdrm/parameters/mode_slots", "r");

        if (f) {
            if (fscanf(f, "%d", &have) != 1)
                have = 0;
            fclose(f);
        }
    }
    if (have < 0)
        have = 0;
    /* --pool：目标尺寸不是"当前生效尺寸"，mode_slots 统计的是后者 ——
     * 不归零的话会因为"当前档已经有 6 个槽"而直接跳过，白跑一趟。*/
    if (pool_arg)
        have = 0;
    P("池里已有 %d 个 %ux%u 的槽（目标 %d）\n", have, w, h, NPOOL);
    if (have >= NPOOL) {
        P("done: %d/%d registered; 池已够，无需补齐\n", have, NPOOL);

        /* 池本来就够，也要 commit —— 否则这次的 mode 永远不会广播出去。
         * --pool 只补槽，不广播。*/
        if (mode_arg && !pool_arg) {
            if (write_param("commit", "1") == 0)
                P("commit: EDID + hotplug 已发出\n");
            else
                P("写 commit 失败: %s\n", strerror(errno));
        }
        close(fd);
        if (lg) fclose(lg);
        return 0;
    }
    need = NPOOL - have;

    for (i = 0; i < need; i++) {
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
    P("done: %d/%d registered; 退出后池由 KPM 持住\n", have + ok, NPOOL);

    /* 槽齐了才广播新尺寸：合成器换档时一定有 buffer 可用。
     * --pool 只补槽，不广播。*/
    if (mode_arg && !pool_arg) {
        if (write_param("commit", "1") == 0)
            P("commit: EDID + hotplug 已发出\n");
        else
            P("写 commit 失败: %s\n", strerror(errno));
    }
    close(fd);
    if (lg) fclose(lg);
    return ok > 0 ? 0 : 2;
}
