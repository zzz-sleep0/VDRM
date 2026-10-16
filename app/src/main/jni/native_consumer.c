/*
 * native_consumer.c — VDRM zero-copy consumer (v0.5)
 *
 * Architecture: KPM registers a virtual /dev/dri/card1, then hooks the
 * global drm_ioctl to intercept MSM_GEM_NEW on renderD128. When the
 * container compositor asks for a GEM buffer, KPM returns one of the APK's
 * pre-registered dma_bufs instead of allocating a fresh one. The compositor
 * renders straight into the APK's gralloc-native pages (DPU can scanout).
 *
 * This APK:
 *   1. dequeues VDR2_BUFS ANativeWindow buffers from its surface
 *   2. extracts each buffer's dma_buf fd
 *   3. registers each fd with KPM via `su -c "kpatch kpm ctl0 ... add_buf=FD"`
 *   4. creates an eventfd + memfd (shm), registers both via ctl0
 *   5. render loop: poll(eventfd) → read shm(slot) → dequeue matching
 *      buffer → queueBuffer → SF → HWC → DPU scanout
 *
 * The buffers are the app surface's own ANativeWindow buffers, so queueBuffer
 * passes the BufferQueue's ownership checks and SurfaceFlinger can display
 * them. The physical pages never move and are never copied.
 *
 * No root is needed per-frame: root (su -c kpatch) is only used at startup
 * to register fds. fd numbers pass across fork() because su inherits the fd
 * table and kpatch resolves the fd in that same inherited context.
 */

#include <android/log.h>
#include <android/native_window.h>
#include <android/native_window_jni.h>
#include <aaudio/AAudio.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <jni.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <sys/eventfd.h>
#include <sys/ioctl.h>
#include <dirent.h>
#include <sys/un.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>

#include "anw_hidden.h"

#include <android/hardware_buffer.h>
#include <android/surface_control.h>

/* VNDK 符号（NDK 无头文件），libnativewindow 导出 */
extern const native_handle_t *AHardwareBuffer_getNativeHandle(const AHardwareBuffer *b);
/* A/B：把 KPM 池里的东西变回 AHB（见 ahb_forge.c）*/
extern AHardwareBuffer *vdrm_ahb_rebuild(int fd0, int fd1, const uint32_t *ints,
                                         uint32_t num_ints, uint32_t w, uint32_t h,
                                         uint32_t stride_bytes, uint32_t hal_fmt,
                                         uint64_t usage);

static ASurfaceControl *g_sc;   /* 从窗口建的 SurfaceControl（显示出口）*/
static jobject g_last_surface;  /* 上次建 SC 用的 Surface（判断是否同一块）*/
static AHardwareBuffer *g_last_ahb;   /* 最后一帧（切前台重贴用）*/
static uint32_t g_last_slot, g_last_w, g_last_h;
static void on_buf_release(void *context, int release_fd);
static pthread_mutex_t g_display_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t g_geometry_lock = PTHREAD_MUTEX_INITIALIZER;
struct submission {
    struct submission *next;
    AHardwareBuffer *ahb;
    uint32_t slot;
    int device_fd, fence_fd;
};
static struct submission *g_submissions;
static int g_release_wake_fd = -1;
static struct submission *submission_create(AHardwareBuffer *ahb, uint32_t slot);
static size_t submission_count(void);
static void sc_geometry(uint32_t mw, uint32_t mh, uint32_t ww, uint32_t wh);

/* 把一块 AHB 贴到 SC 上（可反复调用，用于重贴最后一帧）*/
static void sc_set_geometry(ASurfaceTransaction *t, uint32_t mw, uint32_t mh,
                            uint32_t ww, uint32_t wh)
{
    /* src = KPM 模式(buffer)；dst = 等比缩放(letterbox)后居中放进 Android 窗口。
     * 保持宽高比不拉伸；mode 与屏比例不一致时四周留黑边。*/
    ARect src = {0, 0, (int32_t)mw, (int32_t)mh};
    ARect dst;

    if (!g_sc)
        return;

    if (mw && mh && ww && wh) {
        double s  = (double)ww / (double)mw;
        double s2 = (double)wh / (double)mh;
        int32_t dw, dh, dx, dy;
        if (s2 < s) s = s2;
        dw = (int32_t)((double)mw * s + 0.5);
        dh = (int32_t)((double)mh * s + 0.5);
        if (dw < 1) dw = 1;
        if (dh < 1) dh = 1;
        dx = ((int32_t)ww - dw) / 2;
        dy = ((int32_t)wh - dh) / 2;
        dst.left = dx; dst.top = dy;
        dst.right = dx + dw; dst.bottom = dy + dh;
    } else {
        dst.left = 0; dst.top = 0;
        dst.right = (int32_t)ww; dst.bottom = (int32_t)wh;
    }

    ASurfaceTransaction_setVisibility(t, g_sc,
                                      ASURFACE_TRANSACTION_VISIBILITY_SHOW);
    ASurfaceTransaction_setGeometry(t, g_sc, &src, &dst, 0);
}

static void sc_geometry(uint32_t mw, uint32_t mh, uint32_t ww, uint32_t wh)
{
    if (!g_sc) return;
    ASurfaceTransaction *t = ASurfaceTransaction_create();
    sc_set_geometry(t, mw, mh, ww, wh);
    ASurfaceTransaction_apply(t);
    ASurfaceTransaction_delete(t);
}

static bool sc_latch(AHardwareBuffer *ahb, uint32_t slot,
                     uint32_t mw, uint32_t mh, uint32_t ww, uint32_t wh)
{
    if (!g_sc || !ahb) return false;
    typedef void (*set_buffer_release_fn)(ASurfaceTransaction *, ASurfaceControl *,
                                          AHardwareBuffer *, int, void *,
                                          void (*)(void *, int));
    static set_buffer_release_fn set_buffer_release;
    if (!set_buffer_release)
        set_buffer_release = (set_buffer_release_fn)dlsym(RTLD_DEFAULT,
                                      "ASurfaceTransaction_setBufferWithRelease");
    if (!set_buffer_release) return false;
    struct submission *s = submission_create(ahb, slot);
    if (!s) return false;
    ASurfaceTransaction *t = ASurfaceTransaction_create();
    set_buffer_release(t, g_sc, ahb, -1, s, on_buf_release);
    sc_set_geometry(t, mw, mh, ww, wh);
    ASurfaceTransaction_apply(t);
    ASurfaceTransaction_delete(t);
    return true;
}

/* 把一个 SurfaceControl 从显示树上摘掉：reparent 到 NULL 后它就不再出现在屏幕上，
 * 然后再 release。只 release 不保证立刻从合成里消失 —— 旧图层残留在屏上就是"重影"。*/
static void sc_remove(ASurfaceControl *sc)
{
    ASurfaceTransaction *t;
    if (!sc)
        return;
    t = ASurfaceTransaction_create();
    ASurfaceTransaction_setVisibility(t, sc, ASURFACE_TRANSACTION_VISIBILITY_HIDE);
    /* Clear the buffer through the API 36 nullable-buffer entry point. */
    typedef void (*clear_buffer_fn)(ASurfaceTransaction *, ASurfaceControl *,
                                   AHardwareBuffer *, int, void *,
                                   void (*)(void *, int));
    clear_buffer_fn clear_buffer = (clear_buffer_fn)dlsym(RTLD_DEFAULT,
                                      "ASurfaceTransaction_setBufferWithRelease");
    if (clear_buffer) clear_buffer(t, sc, NULL, -1, NULL, on_buf_release);
    ASurfaceTransaction_reparent(t, sc, NULL);
    ASurfaceTransaction_apply(t);
    ASurfaceTransaction_delete(t);
    ASurfaceControl_release(sc);
}

#define TAG "VDRM"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)
/* 性能：渲染循环里每条 LOGI 都要走 logd，很贵。默认关；调试时改 1。*/
#define VDRM_LOOP_DEBUG 0
#if VDRM_LOOP_DEBUG
#define LDBG(...) LOGI(__VA_ARGS__)
#else
#define LDBG(...) do { } while (0)
#endif
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

/* 池槽上限（= KPM VDRM_POOL_MAX）。必须 >= KPM 池可能出现的 entry 数，
 * 否则池里攒了多批时 App 只 forge 前 N 个，而合成器可能挑到第 N+1 个 ->
 * unknown frame slot -> 黑屏。forge 的是同一批 pool buffer（共享内存，
 * 不额外占显存），放宽到 16 几乎不花代价。*/
#define VDR2_BUFS 16
#define VDRM_W 1920
#define VDRM_H 1080
#define VDR2_CTL0_CMD "kpatch kpm ctl0 vdr2_card1"
#define VDR2_KPATCH_PATH "/data/adb/modules/KPatch-Next/bin/kpatch"
#define VDRM_DEV "/dev/vdrm"

/* ======================== Step 0: SurfaceControl 直贴探针 ========================
 * 目的：验证在 App 自己的进程里，能不能 (a) 从自己的窗口建一个 ASurfaceControl，
 * (b) 把一块 AHardwareBuffer 贴上去显示。若行，hybrid 就能在 App 内完成。 */
static ASurfaceControl *g_sc;
static AHardwareBuffer *g_sc_ahb;

static void sc_probe(ANativeWindow *win)
{
    ASurfaceControl *sc = ASurfaceControl_createFromWindow(win, "vdrm-sctest");
    if (!sc) {
        LOGE("SC: createFromWindow failed errno=%d", errno);
        return;
    }

    AHardwareBuffer_Desc d;
    memset(&d, 0, sizeof(d));
    d.width = 400; d.height = 400; d.layers = 1;
    d.format = AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM;
    d.usage = AHARDWAREBUFFER_USAGE_CPU_WRITE_OFTEN |
              AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE |
              AHARDWAREBUFFER_USAGE_COMPOSER_OVERLAY;
    AHardwareBuffer *ahb = NULL;
    int rc = AHardwareBuffer_allocate(&d, &ahb);
    if (rc != 0 || !ahb) {
        LOGE("SC: AHB alloc rc=%d", rc);
        ASurfaceControl_release(sc);
        return;
    }

    void *p = NULL;
    if (AHardwareBuffer_lock(ahb, AHARDWAREBUFFER_USAGE_CPU_WRITE_OFTEN,
                             -1, NULL, &p) == 0) {
        uint32_t *px = (uint32_t *)p;
        for (int i = 0; i < 400 * 400; i++) px[i] = 0xFFFF00FFu;
        AHardwareBuffer_unlock(ahb, NULL);
    }

    ARect src = {0, 0, 400, 400};
    ARect dst = {0, 0, 400, 400};
    ASurfaceTransaction *t = ASurfaceTransaction_create();
    ASurfaceTransaction_setBuffer(t, sc, ahb, -1);
    ASurfaceTransaction_setVisibility(t, sc,
                                      ASURFACE_TRANSACTION_VISIBILITY_SHOW);
    ASurfaceTransaction_setZOrder(t, sc, 100000);
    ASurfaceTransaction_setGeometry(t, sc, &src, &dst, 0);
    ASurfaceTransaction_apply(t);
    ASurfaceTransaction_delete(t);

    g_sc = sc;
    g_sc_ahb = ahb;
    LOGI("SC: probe applied (magenta 400x400)");
}

/* ---- /dev/vdrm 接口（与 KPM vdrm.c / CONTRACT.md 一致）---- */
struct vdrm_info {
    uint32_t abi_version;
    uint32_t width, height, format, pitch;
    uint32_t pool_capacity;
    uint32_t flags;
    uint32_t reserved;
};
struct vdrm_register_req {
    int32_t  fd;
    uint32_t width, height, format, pitch;
    uint32_t id;
    uint32_t reserved;
    uint64_t user_data;
};
struct vdrm_handle_req {
    uint32_t id;
    uint32_t flags;
};
struct vdrm_input_event {
    uint16_t type;
    uint16_t code;
    int32_t  value;
};
#define VDRM_IOC(dir, type, nr, size) \
    ((unsigned int)(((unsigned int)(dir) << 30) | \
                    ((unsigned int)(size) << 16) | \
                    ((unsigned int)(type) << 8) | (unsigned int)(nr)))
#define VDRM_IOCTL_GET_INFO    VDRM_IOC(2, 'V', 0x00, sizeof(struct vdrm_info))
#define VDRM_IOCTL_REGISTER    VDRM_IOC(3, 'V', 0x01, sizeof(struct vdrm_register_req))
#define VDRM_IOCTL_RETURN      VDRM_IOC(1, 'V', 0x02, sizeof(struct vdrm_handle_req))
#define VDRM_IOCTL_UNREGISTER  VDRM_IOC(1, 'V', 0x03, sizeof(struct vdrm_handle_req))
#define VDRM_IOCTL_SET_EVENTFD VDRM_IOC(1, 'V', 0x04, sizeof(int))
#define VDRM_IOCTL_SET_SHM     VDRM_IOC(1, 'V', 0x05, sizeof(int))
#define VDRM_IOCTL_INPUT       VDRM_IOC(1, 'V', 0x06, sizeof(struct vdrm_input_event))

/* B/A′：KPM 把池里每块的【原始 fd0/fd1 + ints】交给我们（必须与 KPM 一致）*/
#define VDRM_MAX_INTS 40
struct vdrm_pool_info {
    int32_t  fd;       /* fd0：像素（原始 file 的新 fd）*/
    int32_t  fd1;      /* fd1：metadata blob */
    uint32_t id, width, height, pitch, format;
    uint32_t num_ints;
    uint32_t ints[VDRM_MAX_INTS];
    uint32_t flags;
};
#define VDRM_POOL_MAX 16
struct vdrm_pool_list {
    uint32_t count;
    uint32_t reserved;
    struct vdrm_pool_info items[VDRM_POOL_MAX];
};
#define VDRM_IOCTL_GET_POOL    VDRM_IOC(3, 'V', 0x07, sizeof(struct vdrm_pool_list))

/* ---- shared memory layout (must match KPM struct vdr2_frame_info) ---- */
struct vdr2_frame_info {
    uint32_t slot;       /* buffer pool slot index to display */
    uint32_t width;      /* fb width */
    uint32_t height;     /* fb height */
    uint32_t format;     /* DRM fourcc */
    uint32_t seq;        /* frame sequence number */
    uint32_t valid;      /* 0 = no signal, 1 = frame present */
    uint32_t reserved[2];
} __attribute__((packed));

/* ---- 帧事件：/dev/vdrm 的 read() 直接返回这个（必须与 KPM
 *      struct vdrm_frame_event 一致）---- */
struct vdrm_frame_event {
    uint32_t id;            /* 池 id == slot */
    uint32_t sequence;
    uint64_t timestamp_ns;
    uint64_t user_data;
};

/* ---- ANativeWindow hidden API ---- */
static struct anw_api api;
static bool api_loaded;

/* ---- per-buffer bookkeeping ---- */
struct buf_info {
    AHardwareBuffer *ahb;   /* 自己 allocate 的 gralloc buffer（SC 直贴）*/
    int dma_buf_fd;         /* registered with KPM */
    int slot;               /* pool id */
};

struct consumer {
    ANativeWindow *win;
    pthread_t thread;
    atomic_bool running;
    int stop_fd;

    int buf_count;
    struct buf_info bufs[VDR2_BUFS];

    int screen_w;   /* Android 窗口尺寸（显示目标）*/
    int screen_h;
    int mode_w;     /* KPM 决定的输出几何（App 只跟随）*/
    int mode_h;

    /* KPM-APK channels */
    int event_fd;        /* eventfd, KPM → us */
    int shm_fd;          /* memfd */
    volatile struct vdr2_frame_info *shm;  /* mmap'd */
    uint32_t last_seq;

    /* Audio (reserved for KPM audio ctl0; threads only start if fd available) */
    pthread_t play_thr;
    pthread_t cap_thr;
    volatile bool audio_running;
    AAudioStream *play_stream;
    AAudioStream *cap_stream;
};

/* ======================== /dev/vdrm 通道 ========================
 * 新 KPM 用 misc 设备 /dev/vdrm，不再走 `kpatch kpm ctl0`。
 * 注：普通 App 进程可能无权限打开 /dev/vdrm（DAC + SELinux），
 * 所以启动时先用 su 放宽一下权限。
 */
static int g_vdrm_fd = -1;

static int run_su(const char *cmd)
{
    pid_t pid = fork();
    if (pid < 0) { LOGE("su fork failed err=%d", errno); return -errno; }
    if (pid == 0) {
        int dn = open("/dev/null", O_WRONLY);
        if (dn >= 0) { dup2(dn, STDOUT_FILENO); dup2(dn, STDERR_FILENO); close(dn); }
        execl("/system/bin/su", "su", "-c", cmd, (char *)NULL);
        execl("/system/xbin/su", "su", "-c", cmd, (char *)NULL);
        _exit(127);
    }
    int status;
    if (waitpid(pid, &status, 0) < 0) return -errno;
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

static int vdrm_open(void)
{
    if (g_vdrm_fd >= 0) return 0;

    /* 放宽 /dev/vdrm：DAC(chmod) + SELinux 标签(chcon)。内核/KPM 都设不了标签，
     * 只能用户态改；App 有 root，启动时自己 su 一下，省掉宿主手动步骤。 */
    int r = run_su("chmod 666 " VDRM_DEV "; chcon u:object_r:gpu_device:s0 " VDRM_DEV);
    if (r != 0) LOGW("chmod/chcon %s ret=%d", VDRM_DEV, r);

    g_vdrm_fd = open(VDRM_DEV, O_RDWR | O_CLOEXEC | O_NONBLOCK);
    if (g_vdrm_fd < 0) {
        LOGE("open %s failed err=%d", VDRM_DEV, errno);
        return -1;
    }
    LOGI("opened %s fd=%d", VDRM_DEV, g_vdrm_fd);
    return 0;
}

static int vdrm_get_info(struct vdrm_info *info)
{
    if (g_vdrm_fd < 0)
        return -1;
    if (ioctl(g_vdrm_fd, VDRM_IOCTL_GET_INFO, info) < 0) {
        LOGE("GET_INFO failed err=%d", errno);
        return -1;
    }
    return 0;
}

static int vdrm_register(int fd, int w, int h, int format, int stride,
                         uint64_t user_data)
{
    struct vdrm_register_req req;
    memset(&req, 0, sizeof(req));
    req.fd = fd;
    req.width = (uint32_t)w;
    req.height = (uint32_t)h;
    req.format = (uint32_t)format;
    req.pitch = (uint32_t)stride;
    req.user_data = user_data;
    if (ioctl(g_vdrm_fd, VDRM_IOCTL_REGISTER, &req) < 0) {
        LOGE("REGISTER fd=%d failed err=%d", fd, errno);
        return -1;
    }
    return (int)req.id;
}

/* 显示完毕，把池 buffer 归还给 KPM（→ FREE），合成器才能再次渲染进去。 */
static void vdrm_return(int id)
{
    struct vdrm_handle_req req;
    memset(&req, 0, sizeof(req));
    req.id = (uint32_t)id;
    if (g_vdrm_fd >= 0)
        ioctl(g_vdrm_fd, VDRM_IOCTL_RETURN, &req);
}

/* 彻底注销一块池 buffer（App 退出/重配时用），把池槽还给 KPM。 */
static void vdrm_unregister(int id)
{
    struct vdrm_handle_req req;
    memset(&req, 0, sizeof(req));
    req.id = (uint32_t)id;
    if (g_vdrm_fd >= 0)
        ioctl(g_vdrm_fd, VDRM_IOCTL_UNREGISTER, &req);
}

/* SF 用完这块 buffer 时回调 → 归还给 KPM（契中台）。可任意线程。 */
static struct submission *submission_create(AHardwareBuffer *ahb, uint32_t slot)
{
    struct submission *s = calloc(1, sizeof(*s));
    if (!s) return NULL;
    s->device_fd = dup(g_vdrm_fd);
    if (s->device_fd < 0) { free(s); return NULL; }
    pthread_mutex_lock(&g_display_lock);
    s->ahb = ahb; s->slot = slot;
    AHardwareBuffer_acquire(ahb);
    s->next = g_submissions; g_submissions = s;
    pthread_mutex_unlock(&g_display_lock);
    return s;
}

static size_t submission_count(void)
{
    size_t n = 0;
    pthread_mutex_lock(&g_display_lock);
    for (struct submission *p = g_submissions; p; p = p->next) n++;
    pthread_mutex_unlock(&g_display_lock);
    return n;
}

static void *release_wait(void *arg)
{
    struct submission *s = arg;
    if (s->fence_fd >= 0) {
        struct pollfd pfd = { .fd = s->fence_fd, .events = POLLIN };
        int r;
        do { r = poll(&pfd, 1, -1); } while (r < 0 && errno == EINTR);
        if (r < 0 || !(pfd.revents & POLLIN)) {
            LOGE("release fence failed for slot=%u; retaining lease", s->slot);
            close(s->fence_fd);
            return NULL;
        }
        close(s->fence_fd);
    }
    pthread_mutex_lock(&g_display_lock);
    struct submission **p = &g_submissions;
    while (*p && *p != s) p = &(*p)->next;
    if (*p) *p = s->next;
    bool busy = false;
    for (struct submission *other = g_submissions; other; other = other->next)
        if (other->slot == s->slot) { busy = true; break; }
    if (!busy) {
        struct vdrm_handle_req req = { .id = s->slot };
        if (ioctl(s->device_fd, VDRM_IOCTL_RETURN, &req) < 0)
            LOGW("RETURN slot=%u err=%d", s->slot, errno);
    }
    pthread_mutex_unlock(&g_display_lock);
    AHardwareBuffer_release(s->ahb);
    close(s->device_fd);
    if (g_release_wake_fd >= 0) {
        uint64_t wake = 1;
        (void)write(g_release_wake_fd, &wake, sizeof(wake));
    }
    free(s);
    return NULL;
}

static void return_unsubmitted(uint32_t slot)
{
    pthread_mutex_lock(&g_display_lock);
    bool busy = false;
    for (struct submission *p = g_submissions; p; p = p->next)
        if (p->slot == slot) { busy = true; break; }
    if (!busy) vdrm_return((int)slot);
    pthread_mutex_unlock(&g_display_lock);
}

static void on_buf_release(void *context, int release_fd)
{
    struct submission *s = context;
    s->fence_fd = release_fd;
    if (release_fd < 0) { release_wait(s); return; }
    pthread_t thread;
    if (pthread_create(&thread, NULL, release_wait, s) == 0)
        pthread_detach(thread);
    else
        release_wait(s);
}

/* 读 AHB 头 64 像素的校验和，用于判断 buffer 内容是否在变 */
static uint32_t ahb_checksum(AHardwareBuffer *ahb)
{
    void *p = NULL;
    uint32_t sum = 0;
    if (AHardwareBuffer_lock(ahb, AHARDWAREBUFFER_USAGE_CPU_READ_OFTEN,
                             -1, NULL, &p) == 0) {
        uint32_t *px = (uint32_t *)p;
        for (int i = 0; i < 64; i++)
            sum = sum * 31u + px[i];
        AHardwareBuffer_unlock(ahb, NULL);
    } else {
        sum = 0xdeadbeefu;
    }
    return sum;
}

/* 注入一个输入事件给 KPM 的虚拟输入设备 */
static void vdrm_input(uint16_t type, uint16_t code, int32_t value)
{
    struct vdrm_input_event ie;
    ie.type = type; ie.code = code; ie.value = value;
    if (g_vdrm_fd >= 0)
        ioctl(g_vdrm_fd, VDRM_IOCTL_INPUT, &ie);
}

/* ======================== buffer collection ======================== */

/* Extract the dma_buf fd from an ANativeWindowBuffer's native_handle. */
static int anb_dma_buf_fd(const ANativeWindowBuffer *anb)
{
    if (!anb || !anb->handle || anb->handle->numFds < 1) return -1;
    return anb->handle->data[0];
}

static int collect_buffers(struct consumer *c)
{
    ANativeWindow *win = c->win;
    int target = VDR2_BUFS;
    int found = 0;

    LOGI("collecting %d buffers", target);

    for (int attempt = 0; attempt < target * 4 && found < target; attempt++) {
        ANativeWindowBuffer *anb = NULL;
        int fence = -1;
        int dq_ret = api.dequeueBuffer(win, &anb, &fence);
        if (dq_ret != 0 || !anb) {
            LOGE("dequeue failed attempt %d ret=%d", attempt, dq_ret);
            if (fence >= 0) {
                struct pollfd pfd = { .fd = fence, .events = POLLIN };
                poll(&pfd, 1, 1000);
                close(fence);
            }
            usleep(16000);
            continue;
        }
        if (fence >= 0) close(fence);

        int fd = anb_dma_buf_fd(anb);
        if (fd < 0) {
            LOGE("buffer has no dma-buf fd attempt %d", attempt);
            api.cancelBuffer(win, anb, -1);
            continue;
        }

        /* Skip if we already registered this exact buffer */
        bool is_dup = false;
        for (int i = 0; i < found; i++)
            if ((void *)c->bufs[i].ahb == (void *)anb) { is_dup = true; break; }

        if (is_dup) { api.queueBuffer(win, anb, -1); continue; }

        /* dup 一下 fd，然后还 buffer（与原逻辑一致：保证 queueBuffer 后 fd 仍有效）*/
        int reg_fd = dup(fd);
        api.queueBuffer(win, anb, -1);
        if (reg_fd < 0) continue;

        LOGI("  registering: reg_fd=%d (orig %d) -> /dev/vdrm REGISTER", reg_fd, fd);
        int rid = vdrm_register(reg_fd, anb->width, anb->height,
                                anb->format, anb->stride * 4,
                                (uint64_t)(uintptr_t)anb);
        close(reg_fd);
        if (rid < 0)
            continue;

        c->bufs[found].ahb = (AHardwareBuffer *)anb;
        c->bufs[found].dma_buf_fd = fd;
        c->bufs[found].slot = rid;
        LOGI("  buf[%d]: anb=%p fd=%d %dx%d stride=%d id=%d",
             found, (void *)anb, fd, anb->width, anb->height, anb->stride, rid);
        found++;
    }

    if (found < target) {
        LOGE("only collected %d/%d", found, target);
        return -1;
    }

    c->buf_count = found;
    LOGI("collected %d buffers", found);
    return 0;
}

/* 归还并释放当前这一组 buffer：UNREGISTER 所有池槽 + release 所有 AHB。
 * 必须与 alloc_buffers 配对，否则池槽和 gralloc 内存都会泄漏。 */
static void free_buffers(struct consumer *c)
{
    for (int i = 0; i < c->buf_count; i++) {
        if (c->bufs[i].ahb)
            AHardwareBuffer_release(c->bufs[i].ahb);
        c->bufs[i].ahb = NULL;
        c->bufs[i].slot = -1;
        c->bufs[i].dma_buf_fd = -1;
    }
    if (c->buf_count)
        LOGI("freed %d buffers", c->buf_count);
    c->buf_count = 0;
    g_last_ahb = NULL;
    g_last_slot = g_last_w = g_last_h = 0;
}

/* 关掉上一轮的 eventfd/shm，避免反复 surfaceChanged 时泄漏 fd。 */
static void close_channels(struct consumer *c)
{
    if (c->shm) { munmap((void *)c->shm, 4096); c->shm = NULL; }
    if (c->shm_fd >= 0) { close(c->shm_fd); c->shm_fd = -1; }
    if (c->event_fd >= 0) { close(c->event_fd); c->event_fd = -1; }
}

/* A 路线：从 KPM 池 forge 出 AHB（不再自己分配/注册）*/
static int vdrm_get_pool(struct vdrm_pool_list *lst)
{
    if (g_vdrm_fd < 0) return -1;
    memset(lst, 0, sizeof(*lst));
    if (ioctl(g_vdrm_fd, VDRM_IOCTL_GET_POOL, lst) < 0) {
        LOGE("GET_POOL ioctl failed err=%d", errno);
        return -1;
    }
    return (int)lst->count;
}

static int forge_pool(struct consumer *c)
{
    struct vdrm_pool_list lst;
    int n, i;
    const uint32_t usage = AHARDWAREBUFFER_USAGE_CPU_WRITE_OFTEN |
                           AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE |
                           AHARDWAREBUFFER_USAGE_COMPOSER_OVERLAY;

    n = vdrm_get_pool(&lst);
    if (n <= 0) {
        LOGE("forge_pool: KPM pool empty (n=%d)", n);
        return -1;
    }
    c->buf_count = 0;
    for (i = 0; i < n && i < VDR2_BUFS; i++) {
        if (lst.items[i].fd < 0)
            continue;
        /* 中继重建：用 KPM 转发的【原始 fd0/fd1/ints】，不做任何伪造 */
        AHardwareBuffer *ahb = vdrm_ahb_rebuild(lst.items[i].fd,
                                                lst.items[i].fd1,
                                                lst.items[i].ints,
                                                lst.items[i].num_ints,
                                                lst.items[i].width,
                                                lst.items[i].height,
                                                lst.items[i].pitch,
                                                lst.items[i].format, usage);
        close(lst.items[i].fd);
        if (lst.items[i].fd1 >= 0) close(lst.items[i].fd1);
        if (!ahb) { LOGE("forge_pool: rebuild[%d] failed", i); continue; }
        c->bufs[c->buf_count].ahb        = ahb;
        c->bufs[c->buf_count].dma_buf_fd = -1;
        c->bufs[c->buf_count].slot       = (int)lst.items[i].id;
        LOGI("  forged[%d]: id=%u %ux%u pitch=%u ahb=%p", i,
             lst.items[i].id, lst.items[i].width, lst.items[i].height,
             lst.items[i].pitch, (void *)ahb);
        c->buf_count++;
    }
    LOGI("forge_pool: %d forged from KPM pool (of %d)", c->buf_count, n);
    return c->buf_count > 0 ? 0 : -1;
}

/* 自己 allocate VDR2_BUFS 块 gralloc AHB（不靠 SurfaceView），把每块的
 * dma-buf fd 注册进 KPM 池。 */
static int alloc_buffers(struct consumer *c)
{
    AHardwareBuffer_Desc d;
    memset(&d, 0, sizeof(d));
    d.width  = (uint32_t)c->mode_w;
    d.height = (uint32_t)c->mode_h;
    d.layers = 1;
    d.format = AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM;
    d.usage  = AHARDWAREBUFFER_USAGE_CPU_READ_OFTEN |
               AHARDWAREBUFFER_USAGE_CPU_WRITE_OFTEN |
               AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE |
               AHARDWAREBUFFER_USAGE_GPU_COLOR_OUTPUT |
               AHARDWAREBUFFER_USAGE_COMPOSER_OVERLAY;

    c->buf_count = 0;
    for (int i = 0; i < VDR2_BUFS; i++) {
        AHardwareBuffer *ahb = NULL;
        if (AHardwareBuffer_allocate(&d, &ahb) != 0 || !ahb) {
            LOGE("AHB allocate failed at %d", i);
            free_buffers(c);
            return -1;
        }
        AHardwareBuffer_Desc g;
        AHardwareBuffer_describe(ahb, &g);
        const native_handle_t *nh = AHardwareBuffer_getNativeHandle(ahb);
        if (!nh || nh->numFds < 1) {
            LOGE("AHB has no dma-buf fd at %d", i);
            AHardwareBuffer_release(ahb);
            free_buffers(c);
            return -1;
        }
        int fd = nh->data[0];
        int rid = vdrm_register(fd, (int)g.width, (int)g.height,
                                (int)g.format, (int)(g.stride * 4), 0);
        if (rid < 0) {
            AHardwareBuffer_release(ahb);
            free_buffers(c);
            return -1;
        }
        c->bufs[i].ahb = ahb;
        c->bufs[i].dma_buf_fd = fd;
        c->bufs[i].slot = rid;
        c->buf_count = i + 1;

        /* 用不同纯色填满，便于眼睛分辨“屏幕上显示的到底是谁的内容”
         * slot0=红 1=绿 2=蓝 3=黄 4=品红 5=青 */
        {
            static const uint32_t pal[6] = {
                0xFFFF0000u, 0xFF00FF00u, 0xFF0000FFu,
                0xFFFFFF00u, 0xFFFF00FFu, 0xFF00FFFFu
            };
            void *p = NULL;
            if (AHardwareBuffer_lock(ahb,
                    AHARDWAREBUFFER_USAGE_CPU_WRITE_OFTEN, -1, NULL, &p) == 0) {
                uint32_t *px = (uint32_t *)p;
                int n = c->mode_w * c->mode_h;
                for (int k = 0; k < n; k++)
                    px[k] = pal[i % 6];
                AHardwareBuffer_unlock(ahb, NULL);
                LOGI("  AHB[%d] filled color=%08x", i, pal[i % 6]);
            } else {
                LOGE("  AHB[%d] fill lock failed", i);
            }
        }
        LOGI("  buf[%d]: ahb=%p fd=%d %ux%u stride=%u id=%d",
             i, (void *)ahb, fd, g.width, g.height, g.stride, rid);
    }
    c->buf_count = VDR2_BUFS;
    LOGI("allocated %d AHBs %dx%d", c->buf_count, c->mode_w, c->mode_h);
    return 0;
}

/* Return the slot index for a given ANativeWindowBuffer, or -1 if unknown. */
static int buf_slot(struct consumer *c, ANativeWindowBuffer *anb)
{
    for (int i = 0; i < c->buf_count; i++)
        if ((void *)c->bufs[i].ahb == (void *)anb) return c->bufs[i].slot;
    return -1;
}

/* ======================== render loop ======================== */

static void *render_loop(void *arg)
{
    struct consumer *c = arg;
    LOGI("render loop started");
    int loop_count = 0;
    uint64_t stat_ev = 0, stat_show = 0;
    uint32_t prev_show_seq = 0;
    struct vdrm_frame_event pending = {0};
    bool have_pending = false;
    uint64_t stat_deferred = 0;
    time_t stat_t0 = time(NULL);

    while (c->running) {
        loop_count++;

        if (loop_count <= 8)
            LOGI("hb loop=%d running=%d", loop_count, c->running ? 1 : 0);

        if (time(NULL) - stat_t0 >= 2) {
            LOGI("STATS events=%llu shown=%llu deferred=%llu outstanding=%zu over %lds",
                 (unsigned long long)stat_ev, (unsigned long long)stat_show,
                 (unsigned long long)stat_deferred, submission_count(),
                 (long)(time(NULL) - stat_t0));
            stat_ev = 0; stat_show = 0; stat_deferred = 0;
            stat_t0 = time(NULL);
        }

        /* 等 KPM 的帧事件：直接 poll/read /dev/vdrm 自带的队列。
         * （不再依赖 KPM 写 App 的 memfd——那条路内核侧 VFS 会 EACCES。）*/
    struct pollfd pfds[3] = {
        { .fd = g_vdrm_fd, .events = POLLIN },
        { .fd = c->stop_fd, .events = POLLIN },
        { .fd = g_release_wake_fd, .events = POLLIN }
    };
    int pr = poll(pfds, 3, -1);
        if (!c->running || pfds[1].revents) break;
        if (pfds[2].revents) {
            uint64_t wake;
            while (read(g_release_wake_fd, &wake, sizeof(wake)) > 0) {}
        }
        if (pr <= 0) {
            if (pr == 0) LOGW("frame poll timeout loop=%d", loop_count);
            continue;
        }

        /* 排空事件队列，只认最新的一帧（latest wins）。
         * 逐条 FIFO 消费时，一旦有小突发就会去显示已过期的旧帧 → 视觉上"倒带重播"。*/
        struct vdrm_frame_event ev, latest;
        int got = 0;
        uint32_t drained_ids[VDR2_BUFS];
        int drained_count = 0;
        for (int batch = 0; batch < VDR2_BUFS; batch++) {
            if (read(g_vdrm_fd, &ev, sizeof(ev)) != (ssize_t)sizeof(ev))
                break;
            if (got && latest.id != ev.id) {
                bool seen = false;
                for (int i = 0; i < drained_count; i++)
                    if (drained_ids[i] == latest.id) seen = true;
                if (!seen && drained_count < VDR2_BUFS)
                    drained_ids[drained_count++] = latest.id;
            }
            latest = ev;
            got = 1;
        }
        if (got) {
            if (have_pending && pending.id != latest.id)
                return_unsubmitted(pending.id);
            pending = latest;
            have_pending = true;
            for (int i = 0; i < drained_count; i++)
                if (drained_ids[i] != pending.id) return_unsubmitted(drained_ids[i]);
            stat_ev++;
        }
        if (!have_pending) continue;
        /* Keep room for a replacement of the current display buffer. */
        if (submission_count() >= 2) {
            stat_deferred++;
            continue;
        }
        ev = pending;

        uint32_t seq = ev.sequence;
        if (loop_count % 50 == 1)
            LOGI("iter loop=%d seq=%u slot=%u last=%u",
                 loop_count, seq, ev.id, c->last_seq);
        if (seq == c->last_seq) { have_pending = false; continue; }
        uint32_t target_slot = ev.id;
        uint32_t width = (uint32_t)c->mode_w;    /* buffer = KPM 几何 */
        uint32_t height = (uint32_t)c->mode_h;

        /* slot 是 KPM 的全局池 id（跨 App 重启递增），不是数组下标，
         * 必须查表确认它属于我们注册的某块 buffer。*/
        {
            int known = 0;
            for (int i = 0; i < c->buf_count; i++)
                if (c->bufs[i].slot == (int)target_slot) { known = 1; break; }
            if (!known) {
                if (c->buf_count == 0) {
                    /* 池可能刚被 mkpool 建好：重试一次 forge，成不成都不报噪 */
                    if (forge_pool(c) == 0)
                        LOGI("late forge_pool ok: %d bufs", c->buf_count);
                }
                for (int i = 0; i < c->buf_count; i++)
                    if (c->bufs[i].slot == (int)target_slot) { known = 1; break; }
                if (!known) {
                    LOGW("unknown frame slot=%u loop=%d", target_slot, loop_count);
                    continue;
                }
            }
        }

        AHardwareBuffer *ahb = NULL;
        for (int i = 0; i < c->buf_count; i++)
            if (c->bufs[i].slot == (int)target_slot) { ahb = c->bufs[i].ahb; break; }
        if (!ahb) { LOGW("no ahb for slot=%u", target_slot); continue; }

        LDBG("setbuf slot=%u ahb=%p", target_slot, (void *)ahb);
        pthread_mutex_lock(&g_geometry_lock);
        bool shown = sc_latch(ahb, target_slot, width, height,
                              (uint32_t)c->screen_w, (uint32_t)c->screen_h);
        pthread_mutex_unlock(&g_geometry_lock);
        if (!shown) continue;
        c->last_seq = seq;
        have_pending = false;
        if (seq < prev_show_seq)
            LOGW("SEQ BACKWARD prev=%u now=%u", prev_show_seq, seq);
        prev_show_seq = seq;
        LDBG("setbuf applied slot=%u seq=%u", target_slot, seq);
        if (loop_count % 60 == 1)
            LOGI("show slot=%u %ux%u seq=%u loop=%d",
                 target_slot, width, height, seq, loop_count);
        stat_show++;
    }

    LOGI("render loop stopped");
    return NULL;
}

/* ======================== audio (reserved) ========================
 * KPM currently has no audio ctl0 command. These threads are kept for
 * when the KPM audio channel lands; they simply won't start (fd < 0).
 */

static void *audio_play_thread(void *arg)
{
    (void)arg;
    LOGI("audio play thread: no audio channel (KPM audio not yet wired)");
    return NULL;
}

static void *audio_cap_thread(void *arg)
{
    (void)arg;
    LOGI("audio cap thread: no audio channel (KPM audio not yet wired)");
    return NULL;
}

/* ======================== 输入：直接 ioctl 注入内核 ========================
 * 虚拟键鼠（vdrm-kbd / vdrm-ptr）由 vdrm.ko 在内核里注册，生命周期绑在模块上 ——
 * 设备不会被销毁重建，所以容器里合成器手里那个 evdev fd 一直有效，
 * App 反复开关都不影响输入。
 *
 * App 只做一件事：把动作经 ioctl(VDRM_IOCTL_INPUT) 塞给 /dev/vdrm。
 * 无守护进程、无 socketpair，也不搬运节点 —— 设备节点由内核/ueventd 管理，
 * 容器直接按 /dev/input/eventN 逐个绑定。
 * 亚像素累积与滚轮折算原先在守护进程里，现在上移到 App。
 */

/* Linux input 常量（include/uapi/linux/input-event-codes.h）*/
#define IN_EV_KEY    1
#define IN_EV_REL    2
#define IN_REL_X     0
#define IN_REL_Y     1
#define IN_REL_HWHEEL 6
#define IN_REL_WHEEL 8
#define IN_SCROLL_PX_PER_NOTCH 30.0f

static float g_in_mv[2];   /* 亚像素位移累积 */
static float g_in_sc[2];   /* 滚轮累积（0=垂直 1=水平）*/

/* g_vdrm_fd 是【进程级】的，而 vdrm_open() 只在 nativeStart() 里调 —— 于是一个
 * Activity 实例的 onDestroy（nativeDestroy）会把还在前台干活的另一个实例的输入
 * 一起关掉，而且没人会重开它（见 nativeDestroy 处的注释）。
 * 这里自愈：只做一次轻量 open，不跑 su —— 权限/标签在首次 nativeStart 时已经设过，
 * 设备节点没被重建就不需要再设一遍（重建过的话 nativeStart 会再走一遍完整流程）。
 * 失败只报一次、不刷屏：原先这里是静默 return，正是"虚拟键鼠莫名不好使"的元凶。*/
static int vdrm_open_lazy(void)
{
    if (g_vdrm_fd >= 0) return 0;
    g_vdrm_fd = open(VDRM_DEV, O_RDWR | O_CLOEXEC | O_NONBLOCK);
    if (g_vdrm_fd < 0) return -1;
    LOGI("input: lazy re-open %s fd=%d", VDRM_DEV, g_vdrm_fd);
    return 0;
}

static void in_emit(uint16_t type, uint16_t code, int32_t value)
{
    struct vdrm_input_event ie;
    static int warned;

    if (g_vdrm_fd < 0 && vdrm_open_lazy() < 0) {
        if (!warned) {
            warned = 1;
            LOGW("input: %s 打不开 err=%d —— 事件被丢弃", VDRM_DEV, errno);
        }
        return;
    }
    warned = 0;
    ie.type = type; ie.code = code; ie.value = value;
    if (ioctl(g_vdrm_fd, VDRM_IOCTL_INPUT, &ie) < 0)
        LOGW("input: ioctl type=%u code=%u err=%d", type, code, errno);
}

static void in_key(int code, int down)
{
    in_emit(IN_EV_KEY, (uint16_t)code, down ? 1 : 0);
}

static void in_btn(int btn, int pressed)
{
    in_emit(IN_EV_KEY, (uint16_t)btn, pressed ? 1 : 0);
}

static void in_motion(float dx, float dy)
{
    int ix, iy;
    g_in_mv[0] += dx;
    g_in_mv[1] += dy;
    ix = (int)g_in_mv[0];
    iy = (int)g_in_mv[1];
    if (ix) { g_in_mv[0] -= (float)ix; in_emit(IN_EV_REL, IN_REL_X, ix); }
    if (iy) { g_in_mv[1] -= (float)iy; in_emit(IN_EV_REL, IN_REL_Y, iy); }
}

static void in_scroll(int axis, float v)
{
    int idx = axis ? 1 : 0;
    uint16_t rel = axis ? IN_REL_HWHEEL : IN_REL_WHEEL;

    g_in_sc[idx] += v;
    while (g_in_sc[idx] >= IN_SCROLL_PX_PER_NOTCH) {
        g_in_sc[idx] -= IN_SCROLL_PX_PER_NOTCH;
        in_emit(IN_EV_REL, rel, 1);
    }
    while (g_in_sc[idx] <= -IN_SCROLL_PX_PER_NOTCH) {
        g_in_sc[idx] += IN_SCROLL_PX_PER_NOTCH;
        in_emit(IN_EV_REL, rel, -1);
    }
}

/* ======================== JNI (static: event helpers) ========================
 * 输入走内核注入（见上面的 in_key / in_motion / in_btn / in_scroll）。
 */

JNIEXPORT jint JNICALL
Java_com_vdrm_consumer_Native_nativeSendKey(JNIEnv *env, jclass clazz, jint code, jboolean down)
{
    (void)env; (void)clazz;
    in_key((int)code, down);
    return 0;
}

JNIEXPORT jint JNICALL
Java_com_vdrm_consumer_Native_nativeSendMotion(JNIEnv *env, jclass clazz, jint dx, jint dy)
{
    (void)env; (void)clazz;
    in_motion((float)dx, (float)dy);
    return 0;
}

JNIEXPORT jint JNICALL
Java_com_vdrm_consumer_Native_nativeSendBtn(JNIEnv *env, jclass clazz, jint btn, jboolean pressed)
{
    (void)env; (void)clazz;
    in_btn((int)btn, pressed);
    return 0;
}

JNIEXPORT jint JNICALL
Java_com_vdrm_consumer_Native_nativeSendScroll(JNIEnv *env, jclass clazz, jint axis, jint val)
{
    (void)env; (void)clazz;
    in_scroll((int)axis, (float)val);
    return 0;
}

/* ======================== JNI (instance: lifecycle) ======================== */

#define STATE(h) ((struct consumer *)(uintptr_t)(h))

JNIEXPORT jlong JNICALL
Java_com_vdrm_consumer_Native_nativeCreate(JNIEnv *env, jclass clazz)
{
    (void)env; (void)clazz;
    signal(SIGPIPE, SIG_IGN);   /* 写死掉的输入守护进程 socketpair 时不要崩 */
    struct consumer *c = calloc(1, sizeof(*c));
    if (!c) return 0;
    atomic_init(&c->running, false);
    c->stop_fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (c->stop_fd < 0) { free(c); return 0; }
    c->event_fd = -1;
    c->shm_fd = -1;
    LOGI("instance %p created", (void *)c);
    return (jlong)(uintptr_t)c;
}

JNIEXPORT void JNICALL
Java_com_vdrm_consumer_Native_nativeDestroy(JNIEnv *env, jclass clazz, jlong handle)
{
    struct consumer *c = STATE(handle);
    if (!c) return;

    if (c->running) {
        c->running = false;
        uint64_t wake = 1;
        write(c->stop_fd, &wake, sizeof(wake));
        pthread_join(c->thread, NULL);
        while (read(c->stop_fd, &wake, sizeof(wake)) > 0) {}
    }
    sc_remove(g_sc);
    g_sc = NULL;
    if (g_last_surface) {
        (*env)->DeleteGlobalRef(env, g_last_surface);
        g_last_surface = NULL;
    }
    /* 输入设备归内核（vdrm.ko）所有，App 退出不碰它 ——
     * 这样容器里的合成器不会因为 App 重启而丢掉输入。*/
    if (c->win) {
        ANativeWindow_release(c->win);
        c->win = NULL;
    }
    free_buffers(c);        /* UNREGISTER 池槽 + release AHB */
    close_channels(c);
    /* 这里【不】关 g_vdrm_fd：它是进程级的，可能还有别的 MainActivity 实例
     * （manifest 是 standard/singleTask 之前可以并存多个）在用；而且它是输入
     * 注入唯一的依赖 —— 关了就等于把别人正在用的输入一起掐了，还查不出来
     * （in_emit 以前是静默 return）。进程退出时内核自然会收这个 fd。*/
    LOGI("instance %p destroyed", (void *)c);
    if (c->stop_fd >= 0) close(c->stop_fd);
    free(c);
}

/* App 只负责一件事：池不存在时把 mkpool 拉起来（其余不管）。
 * mkpool 跑完就退，池由内核持住；起不来/失败都不阻塞 App。
 *
 * 路径就是模块目录里的那份（模块 id 固定为 vdrm-lkm）—— 二进制内嵌在模块里，
 * 不再往 /data/wayland 拷第二份（那是守护进程时代留下的路径，已退役）。*/
#define VDRM_MKPOOL "/data/adb/modules/vdrm-lkm/vdrm_mkpool"

static void ensure_pool_spawn(void)
{
    struct vdrm_pool_list lst;
    int n, usable = 0, i;
    pid_t pid;

    n = vdrm_get_pool(&lst);
    for (i = 0; i < n; i++)
        if (lst.items[i].fd >= 0) usable++;
    if (usable > 0)
        return;   /* 有可用的真池（带原始 fd）就不管 */
    /* 池空，或只剩 HEAP 回退登记的“无 fd”槽：都要重建（mkpool 会先清池）*/
    LOGI("ensure_pool: 池不可用(n=%d usable=%d) -> 拉起 mkpool", n, usable);
    pid = fork();
    if (pid == 0) {
        execl("/system/bin/su", "su", "-c", VDRM_MKPOOL, (char *)NULL);
        _exit(127);
    }
}

JNIEXPORT void JNICALL
Java_com_vdrm_consumer_Native_nativeStart(JNIEnv *env, jclass clazz,
                                          jlong handle, jobject surface)
{
    struct consumer *c = STATE(handle);
    if (!c) return;
    int same_surface = 0;   /* 纯 resize 时保留 buffer，不重 forge（避免卡在上一帧）*/

    if (vdrm_open() < 0) {
        LOGE("vdrm_open failed (no /dev/vdrm access?)");
        return;
    }
    /* A live Surface resize changes geometry only, not buffer ownership. */
    if (c->running && g_sc && g_last_surface &&
        (*env)->IsSameObject(env, g_last_surface, surface)) {
        pthread_mutex_lock(&g_geometry_lock);
        c->screen_w = ANativeWindow_getWidth(c->win);
        c->screen_h = ANativeWindow_getHeight(c->win);
        sc_geometry(c->mode_w, c->mode_h, c->screen_w, c->screen_h);
        pthread_mutex_unlock(&g_geometry_lock);
        return;
    }
    /* 输入守护进程改到下面随池一起拉起（启动即建），不再等第一次按键。*/

        if (c->running) {
            c->running = false;
            uint64_t wake = 1;
            write(c->stop_fd, &wake, sizeof(wake));
            pthread_join(c->thread, NULL);
            while (read(c->stop_fd, &wake, sizeof(wake)) > 0) {}
        }
        /* Resize is handled above without stopping the render loop. */
    close_channels(c);

    /* 分清"同一块 surface 的纯尺寸变化"和"surface 被销毁重建"：
     *   - 纯 resize（小窗→全屏）：必须保留同一个 SurfaceControl，只更新几何(dst)；
     *     重建会留下旧图层 → 屏幕上的"重影"（要 reparent 到 NULL 才会消失）。
     *   - surface 变了（放后台再回来）：旧 window/SC 已作废，才重建。*/
    {
        jobject gs = (*env)->NewGlobalRef(env, surface);
        same_surface = (g_last_surface != NULL &&
                        (*env)->IsSameObject(env, g_last_surface, gs));
        if (g_last_surface)
            (*env)->DeleteGlobalRef(env, g_last_surface);
        g_last_surface = gs;

        if (!same_surface) {
            if (c->win) { ANativeWindow_release(c->win); c->win = NULL; }
            sc_remove(g_sc);
            g_sc = NULL;
            c->win = ANativeWindow_fromSurface(env, surface);
            if (!c->win) {
                LOGE("ANativeWindow_fromSurface failed");
                return;
            }
            g_sc = ASurfaceControl_createFromWindow(c->win, "vdrm");
            if (!g_sc) {
                LOGE("ASurfaceControl_createFromWindow failed errno=%d", errno);
                return;
            }
            LOGI("surface changed -> rebuilt SurfaceControl");
        } else {
            LOGI("pure resize -> keep SurfaceControl, re-latch geometry");
        }
    }
    if (!c->win) {
        LOGE("no window after surface change");
        return;
    }

    c->screen_w = ANativeWindow_getWidth(c->win);
    c->screen_h = ANativeWindow_getHeight(c->win);
    /* 几何从 KPM 来（App 只跟随）；取不到就退回窗口尺寸 */
    {
        struct vdrm_info info;
        memset(&info, 0, sizeof(info));
        if (vdrm_get_info(&info) == 0 && info.width && info.height) {
            if (c->buf_count > 0 &&
                (c->mode_w != (int)info.width || c->mode_h != (int)info.height)) {
                LOGI("mode changed %dx%d -> %ux%u, rebuild buffers",
                     c->mode_w, c->mode_h, info.width, info.height);
                free_buffers(c);
            }
            c->mode_w = (int)info.width;
            c->mode_h = (int)info.height;
            LOGI("KPM mode %dx%d fmt=%08x pitch=%u", c->mode_w, c->mode_h,
                 info.format, info.pitch);
        } else {
            c->mode_w = c->screen_w;
            c->mode_h = c->screen_h;
            LOGW("GET_INFO failed, fallback window %dx%d", c->mode_w, c->mode_h);
        }
    }

    /* A cached AHB may already have been returned; wait for a fresh frame. */

    /* Process-wide wake fd outlives asynchronous release callbacks. */
    if (g_release_wake_fd < 0)
        g_release_wake_fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (g_release_wake_fd < 0) {
        LOGE("release eventfd failed err=%d", errno);
        return;
    }

    /* Create the eventfd (KPM → us) and shm (memfd) channels. */
    c->event_fd = eventfd(0, EFD_NONBLOCK);
    if (c->event_fd < 0) {
        LOGE("eventfd failed err=%d", errno);
        return;
    }
    c->shm_fd = syscall(SYS_memfd_create, "vdr2_shm", MFD_CLOEXEC);
    if (c->shm_fd < 0) {
        LOGE("memfd_create failed err=%d", errno);
        return;
    }
    if (ftruncate(c->shm_fd, 4096) < 0) {
        LOGE("ftruncate failed err=%d", errno);
        return;
    }
    void *mapped = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED,
                        c->shm_fd, 0);
    if (mapped == MAP_FAILED) {
        LOGE("mmap shm failed err=%d", errno);
        return;
    }
    c->shm = (volatile struct vdr2_frame_info *)mapped;
    memset((void *)c->shm, 0, 4096);

    /* Register the channels with KPM (each one is a single su -c). */
    LOGI("registering channels: event_fd=%d shm_fd=%d", c->event_fd, c->shm_fd);
    int r1 = ioctl(g_vdrm_fd, VDRM_IOCTL_SET_EVENTFD, (unsigned long)c->event_fd);
    int r2 = ioctl(g_vdrm_fd, VDRM_IOCTL_SET_SHM, (unsigned long)c->shm_fd);
    LOGI("SET_EVENTFD=%d SET_SHM=%d", r1, r2);

    /* B/A′：池由用户态 helper 在 KPM 加载后建好，App 只负责【重建 + 上屏】。
     * 纯 resize 时 buffer/池都没变 → 不 free/不重 forge，保住 g_last_ahb，
     * 下面用新几何重贴最后一帧 → 不会“卡在上一帧”。*/
    /* 虚拟键鼠：设备由 vdrm.ko 在内核注册，节点就在 /dev/input/eventN，
     * 由内核/ueventd 管理（注册时建、注销时删）。容器直接按节点绑定即可，
     * 不再需要 App 搬运 —— 也就不会留下悬空节点。*/

    if (!same_surface || c->buf_count <= 0) {
        ensure_pool_spawn();          /* 池空就先拉起 mkpool（异步，不等）*/
        free_buffers(c);
        if (forge_pool(c) < 0)
            LOGE("forge_pool 暂不可用（池未就绪？）— 渲染循环会自动重试");
    } else {
        LOGI("pure resize -> keep %d buffers (no re-forge), re-latch to new geometry",
             c->buf_count);
    }

    /* Start the render loop. */
    c->running = true;
    pthread_create(&c->thread, NULL, render_loop, c);


    LOGI("started (mode %dx%d) win %dx%d %d bufs",
         c->mode_w, c->mode_h, c->screen_w, c->screen_h, c->buf_count);
}

JNIEXPORT void JNICALL
Java_com_vdrm_consumer_Native_nativeStop(JNIEnv *env, jclass clazz, jlong handle)
{
    struct consumer *c = STATE(handle);
    if (!c) return;

    if (c->running) {
        c->running = false;
        uint64_t wake = 1;
        write(c->stop_fd, &wake, sizeof(wake));
        pthread_join(c->thread, NULL);
        while (read(c->stop_fd, &wake, sizeof(wake)) > 0) {}
    }

    /* Notify KPM to stop signaling (sets eventfd ctx to NULL) */
    if (g_vdrm_fd >= 0) {
        ioctl(g_vdrm_fd, VDRM_IOCTL_SET_EVENTFD, 0);
    }

    sc_remove(g_sc);
    g_sc = NULL;
    if (g_last_surface) {
        (*env)->DeleteGlobalRef(env, g_last_surface);
        g_last_surface = NULL;
    }
    if (c->win) { ANativeWindow_release(c->win); c->win = NULL; }
    c->last_seq = 0;
    LOGI("stopped");
}

JNIEXPORT void JNICALL
Java_com_vdrm_consumer_Native_nativeStartAudio(JNIEnv *env, jclass clazz, jlong handle)
{
    struct consumer *c = STATE(handle);
    if (!c) return;
    /* Audio channel not wired in KPM yet — reserved. */
    (void)c;
}

JNIEXPORT void JNICALL
Java_com_vdrm_consumer_Native_nativeStopAudio(JNIEnv *env, jclass clazz, jlong handle)
{
    struct consumer *c = STATE(handle);
    if (!c) return;
    (void)c;
}