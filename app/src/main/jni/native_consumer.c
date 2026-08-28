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
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <sys/eventfd.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>

#include "anw_hidden.h"

#define TAG "VDRM"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

#define VDR2_BUFS 4
#define VDR2_CTL0_CMD "kpatch kpm ctl0 vdr2_card1"
#define VDR2_KPATCH_PATH "/data/adb/modules/KPatch-Next/bin/kpatch"

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

/* ---- ANativeWindow hidden API ---- */
static struct anw_api api;
static bool api_loaded;

/* ---- per-buffer bookkeeping ---- */
struct buf_info {
    ANativeWindowBuffer *anb;
    int dma_buf_fd;      /* the fd we registered with KPM */
    int slot;            /* pool slot index */
};

struct consumer {
    ANativeWindow *win;
    pthread_t thread;
    volatile bool running;

    int buf_count;
    struct buf_info bufs[VDR2_BUFS];

    int screen_w;
    int screen_h;

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

/* ======================== root helper ========================
 * Runs a single command as root. fd numbers are inherited across fork(),
 * so `su -c "kpatch ... add_buf=FD"` sees the same fd table as we do.
 */
static int kpatch_ctl0(const char *fmt, ...)
{
    char cmd[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(cmd, sizeof(cmd), fmt, ap);
    va_end(ap);

    /* full command: kpatch kpm ctl0 vdr2_card1 "cmd" */
    char full[600];
    snprintf(full, sizeof(full), "%s \"%s\"", VDR2_CTL0_CMD, cmd);

    pid_t pid = fork();
    if (pid < 0) { LOGE("ctl0 fork failed err=%d", errno); return -errno; }
    if (pid == 0) {
        /* redirect su output to /dev/null (kpatch prints a banner) */
        int dn = open("/dev/null", O_WRONLY);
        if (dn >= 0) { dup2(dn, STDOUT_FILENO); dup2(dn, STDERR_FILENO); close(dn); }
        execl("/system/bin/su", "su", "-c", full, (char *)NULL);
        /* fallback exec paths */
        execl("/system/xbin/su", "su", "-c", full, (char *)NULL);
        _exit(127);
    }
    int status;
    if (waitpid(pid, &status, 0) < 0) { LOGE("ctl0 waitpid failed err=%d", errno); return -errno; }
    int ret = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    if (ret != 0)
        LOGW("ctl0 '%s' ret=%d", cmd, ret);
    return ret;
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
            if (c->bufs[i].anb == anb) { is_dup = true; break; }

        /* Register the fd with KPM (fd number valid in our process and in
         * the forked su child — fd table is inherited). */
        int reg_fd = is_dup ? -1 : dup(fd);

        /* Return the buffer to the surface pool before the root call —
         * KPM's dma_buf_get takes its own kernel ref, so the fd stays alive
         * even after we queue the buffer back. */
        api.queueBuffer(win, anb, -1);

        if (is_dup) continue;
        if (reg_fd < 0) continue;

        LOGI("  registering: dup_fd=%d (orig fd=%d) → kpatch add_buf", reg_fd, fd);
        int ret = kpatch_ctl0("add_buf=%d", reg_fd);
        close(reg_fd);
        if (ret != 0) {
            LOGE("add_buf failed fd=%d ret=%d (fd may not have survived su fork)", fd, ret);
            continue;
        }

        c->bufs[found].anb = anb;
        c->bufs[found].dma_buf_fd = fd;
        c->bufs[found].slot = found;
        LOGI("  buf[%d]: anb=%p fd=%d %dx%d stride=%d slot=%d",
             found, (void *)anb, fd, anb->width, anb->height, anb->stride, found);
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

/* Return the slot index for a given ANativeWindowBuffer, or -1 if unknown. */
static int buf_slot(struct consumer *c, ANativeWindowBuffer *anb)
{
    for (int i = 0; i < c->buf_count; i++)
        if (c->bufs[i].anb == anb) return c->bufs[i].slot;
    return -1;
}

/* ======================== render loop ======================== */

static void *render_loop(void *arg)
{
    struct consumer *c = arg;
    LOGI("render loop started");
    int loop_count = 0;

    while (c->running) {
        loop_count++;

        /* Wait for KPM to ring the bell (a new frame is ready). */
        struct pollfd pfd = { .fd = c->event_fd, .events = POLLIN };
        int pr = poll(&pfd, 1, 5000);
        if (pr <= 0) {
            if (pr == 0) LOGW("eventfd timeout loop=%d", loop_count);
            continue;
        }

        /* Drain the eventfd counter. */
        uint64_t cnt;
        while (read(c->event_fd, &cnt, sizeof(cnt)) == sizeof(cnt)) {}

        /* Read the shared-memory frame info (seqlock: data is written before
         * seq; KPM does smp_wmb before bumping seq). */
        volatile struct vdr2_frame_info *info = c->shm;
        uint32_t seq = info->seq;
        if (seq == c->last_seq) continue;  /* same frame, skip */
        __sync_synchronize();
        uint32_t target_slot = info->slot;
        uint32_t width = info->width;
        uint32_t height = info->height;
        c->last_seq = seq;

        if (!info->valid || target_slot >= (uint32_t)c->buf_count) {
            LOGW("bad frame slot=%u valid=%d loop=%d", target_slot, info->valid, loop_count);
            continue;
        }

        /* Dequeue until we get the buffer that belongs to the target slot. */
        for (int retries = 0; retries < VDR2_BUFS * 2; retries++) {
            ANativeWindowBuffer *anb = NULL;
            int fence = -1;
            int dq_ret = api.dequeueBuffer(c->win, &anb, &fence);
            if (dq_ret != 0 || !anb) {
                usleep(16000);
                continue;
            }
            if (fence >= 0) {
                struct pollfd pf = { .fd = fence, .events = POLLIN };
                poll(&pf, 1, 1000);
                close(fence);
            }
            if (buf_slot(c, anb) == (int)target_slot) {
                /* This is the buffer the compositor just rendered into.
                 * Queue it → SF → HWC → DPU scanout. */
                if (loop_count % 60 == 1)
                    LOGI("show slot=%u %ux%u seq=%u loop=%d",
                         target_slot, width, height, seq, loop_count);
                api.queueBuffer(c->win, anb, -1);
                break;
            }
            api.cancelBuffer(c->win, anb, -1);
        }
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

/* ======================== JNI (static: event helpers) ========================
 * Input events previously went through the socket proxy to /dev/vdr2ctl.
 * The new KPM has no input ctl0 channel yet — these are no-ops pending
 * the container input path (KPM card1 /dev/input device or ctl0).
 */

JNIEXPORT jint JNICALL
Java_com_vdrm_consumer_Native_nativeSendKey(JNIEnv *env, jclass clazz, jint code, jboolean down)
{
    (void)env; (void)clazz; (void)code; (void)down;
    return 0;
}

JNIEXPORT jint JNICALL
Java_com_vdrm_consumer_Native_nativeSendMotion(JNIEnv *env, jclass clazz, jint dx, jint dy)
{
    (void)env; (void)clazz; (void)dx; (void)dy;
    return 0;
}

JNIEXPORT jint JNICALL
Java_com_vdrm_consumer_Native_nativeSendBtn(JNIEnv *env, jclass clazz, jint btn, jboolean pressed)
{
    (void)env; (void)clazz; (void)btn; (void)pressed;
    return 0;
}

JNIEXPORT jint JNICALL
Java_com_vdrm_consumer_Native_nativeSendScroll(JNIEnv *env, jclass clazz, jint axis, jint val)
{
    (void)env; (void)clazz; (void)axis; (void)val;
    return 0;
}

/* ======================== JNI (instance: lifecycle) ======================== */

#define STATE(h) ((struct consumer *)(uintptr_t)(h))

JNIEXPORT jlong JNICALL
Java_com_vdrm_consumer_Native_nativeCreate(JNIEnv *env, jclass clazz)
{
    (void)env; (void)clazz;
    struct consumer *c = calloc(1, sizeof(*c));
    if (!c) return 0;
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
        pthread_join(c->thread, NULL);
    }
    if (c->win) {
        ANativeWindow_release(c->win);
        c->win = NULL;
    }
    if (c->shm) {
        munmap((void *)c->shm, 4096);
        c->shm = NULL;
    }
    if (c->shm_fd >= 0) { close(c->shm_fd); c->shm_fd = -1; }
    if (c->event_fd >= 0) { close(c->event_fd); c->event_fd = -1; }
    LOGI("instance %p destroyed", (void *)c);
    free(c);
}

JNIEXPORT void JNICALL
Java_com_vdrm_consumer_Native_nativeStart(JNIEnv *env, jclass clazz,
                                          jlong handle, jobject surface)
{
    struct consumer *c = STATE(handle);
    if (!c) return;

    if (!api_loaded) {
        if (anw_api_load(&api) < 0) {
            LOGE("failed to load ANativeWindow hidden API");
            return;
        }
        api_loaded = true;
    }

    if (c->running) {
        c->running = false;
        pthread_join(c->thread, NULL);
    }
    if (c->win) {
        ANativeWindow_release(c->win);
        c->win = NULL;
    }

    c->win = ANativeWindow_fromSurface(env, surface);
    if (!c->win) {
        LOGE("ANativeWindow_fromSurface failed");
        return;
    }

    c->screen_w = ANativeWindow_getWidth(c->win);
    c->screen_h = ANativeWindow_getHeight(c->win);

    int min_ud = 0;
    api.query(c->win, ANATIVEWINDOW_QUERY_MIN_UNDEQUEUED_BUFFERS, &min_ud);

    /* Set up the surface: VDR2_BUFS buffers, matching the compositor's
     * frame rate / resolution so round-robin gives every slot a home. */
    ANativeWindow_setBuffersGeometry(c->win, c->screen_w, c->screen_h,
                                     AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM);
    int sbc_ret = api.setBufferCount(c->win, VDR2_BUFS);
    LOGI("setBuffersGeometry done, setBufferCount=%d (min_ud=%d)",
         sbc_ret, min_ud);

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
    int r1 = kpatch_ctl0("set_eventfd=%d", c->event_fd);
    int r2 = kpatch_ctl0("set_shm=%d", c->shm_fd);
    LOGI("set_eventfd=%d set_shm=%d", r1, r2);

    /* Collect the surface buffers and register their dma_buf fds. */
    if (collect_buffers(c) < 0) {
        LOGE("collect_buffers failed");
        ANativeWindow_release(c->win);
        c->win = NULL;
        return;
    }

    /* Start the render loop. */
    c->running = true;
    pthread_create(&c->thread, NULL, render_loop, c);

    LOGI("started %dx%d %d bufs", c->screen_w, c->screen_h, c->buf_count);
}

JNIEXPORT void JNICALL
Java_com_vdrm_consumer_Native_nativeStop(JNIEnv *env, jclass clazz, jlong handle)
{
    struct consumer *c = STATE(handle);
    if (!c) return;

    if (c->running) {
        c->running = false;
        pthread_join(c->thread, NULL);
    }

    /* Notify KPM to stop signaling (sets eventfd_ctx to NULL, stops vblank writes) */
    if (c->event_fd >= 0) {
        kpatch_ctl0("set_eventfd=0");
    }

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