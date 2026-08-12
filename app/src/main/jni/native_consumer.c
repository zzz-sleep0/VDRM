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

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <sys/eventfd.h>
#include <sys/ioctl.h>
#include <sys/wait.h>

#include "anw_hidden.h"
#include "vdr2_proto.h"

#define TAG "VDRM"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

#define VDR2CTL_PATH "/dev/vdr2ctl"
#define MAX_BUFS 6
#define VDR2_SLOTS 3

/* ---- Guard booth protocol (ioctl on /dev/vdr2ctl) ---- */

#define VDR2_IOC_NONE 0
#define VDR2_IOC_WRITE 1
#define VDR2_IOC_READ 2

struct vdr2_frame_io { int fd; int slot; unsigned long long seq; };
struct vdr2_ev_io    { int type, code, value, x, y; };
struct vdr2_reg_io   { int fd, slot, efd; unsigned stride; };

/* ---- V4 slot protocol ----
 * Our zero-copy path is: REGISTER_BUF (lend ANativeWindow buffers to the
 * KPM) + FRAME.slot (ask which buffer the container just flipped into). The
 * V3 REGISTER_CHANNELS / GET_FENCE / CLEAR ioctls below are kept for future
 * anland alignment but not actively driven by the current render loop.
 */
struct vdr3_shm_page {
    unsigned long long seq;
    unsigned int fb_id;
    unsigned int width, height;
    int consumer_state;
    unsigned int current_fence;
    unsigned int pad;
};
struct vdr2_ch_io {
    int shm_fd;
    int bell_fd;
    int fence_fd;
    unsigned int width, height;
};

#define VDR2_IOC(cmd)        ((cmd) & 0xFF)
#define VDR2_IOC_BEGIN          (0x56u << 8 | 0)
#define VDR2_IOC_PRESENT        (VDR2_IOC_WRITE << 30 | 0x56u << 8 | 1 | sizeof(int) << 16)
#define VDR2_IOC_FRAME          (VDR2_IOC_READ  << 30 | 0x56u << 8 | 2 | sizeof(struct vdr2_frame_io) << 16)
#define VDR2_IOC_REGISTER_BUF   (VDR2_IOC_WRITE << 30 | 0x56u << 8 | 3 | sizeof(struct vdr2_reg_io) << 16)
#define VDR2_IOC_CLEAR_BUFS     (0x56u << 8 | 4)
#define VDR2_IOC_EV             (VDR2_IOC_WRITE << 30 | 0x56u << 8 | 5 | sizeof(struct vdr2_ev_io) << 16)
#define VDR2_IOC_AUDIO_PLAY     (VDR2_IOC_READ  << 30 | 0x56u << 8 | 6 | sizeof(int) << 16)
#define VDR2_IOC_AUDIO_CAP      (VDR2_IOC_READ  << 30 | 0x56u << 8 | 7 | sizeof(int) << 16)
#define VDR2_IOC_AUDIO_PLAY_RECV (VDR2_IOC_READ << 30 | 0x56u << 8 | 8 | sizeof(int) << 16)
#define VDR2_IOC_AUDIO_CAP_RECV  (VDR2_IOC_READ << 30 | 0x56u << 8 | 9 | sizeof(int) << 16)
#define VDR2_IOC_CHANNELS        (VDR2_IOC_WRITE << 30 | 0x56u << 8 | 10 | sizeof(struct vdr2_ch_io) << 16)
#define VDR2_IOC_FENCE           (VDR2_IOC_READ  << 30 | 0x56u << 8 | 11 | sizeof(struct vdr2_frame_io) << 16)
#define VDR2_IOC_CLEAR           (0x56u << 8 | 12)

/* Event types (must match KPM) */
#define VDR2_EV_KEY    1
#define VDR2_EV_BUTTON 2
#define VDR2_EV_MOTION 3
#define VDR2_EV_SCROLL 4

static int vdr2_bell = -1;
static int vdr2_bell_registered;

/* ---- vdr2d root proxy (named unix socket + SCM_RIGHTS) ----
 * untrusted_app cannot open /dev/vdr2ctl. The app binds a named
 * SOCK_SEQPACKET socket in its files dir, spawns the vdr2d daemon via
 * `su -c "<files>/vdr2d --connect <sockpath>"` (u:r:ksu:s0); the daemon
 * connects, opens the device itself and runs every guard-booth ioctl.
 * Eventfd / dma-buf arguments are passed with SCM_RIGHTS, out-fds
 * (AUDIO_*) come back the same way. A named socket (not socketpair fd
 * passing) is required because the root executor (su / Sui) does not
 * inherit the app's fd. */

#define VDR2_IOC_NR(cmd) ((cmd) & 0xFF)

#define VDR2_CONNECT_TIMEOUT_MS 1500

struct vdr2_proxy {
    int sock;
    int listen;
    pid_t child;
    char daemon_path[512];
    char sock_path[512];
};

static struct vdr2_proxy proxy = {
    .sock = -1, .listen = -1, .child = -1, .daemon_path = {0},
};

static void proxy_down(void)
{
    if (proxy.sock >= 0) close(proxy.sock);
    proxy.sock = -1;
    if (proxy.child > 0) {
        kill(proxy.child, SIGKILL);
        waitpid(proxy.child, NULL, 0);
        proxy.child = -1;
    }
    if (proxy.listen >= 0) {
        close(proxy.listen);
        proxy.listen = -1;
        unlink(proxy.sock_path);
    }
    vdr2_bell_registered = 0;
}

static int proxy_spawn(void)
{
    if (proxy.sock >= 0) return 0;
    if (!proxy.daemon_path[0]) {
        LOGE("proxy: no daemon path");
        return -EINVAL;
    }

    /* Clean up any previous daemon and socket before spawning a new one */
    if (proxy.child > 0) {
        kill(proxy.child, SIGKILL);
        waitpid(proxy.child, NULL, 0);
        proxy.child = -1;
    }
    if (proxy.listen >= 0) {
        close(proxy.listen);
        proxy.listen = -1;
        unlink(proxy.sock_path);
    }

    if (proxy.listen < 0) {
        snprintf(proxy.sock_path, sizeof(proxy.sock_path), "%s.sock",
                 proxy.daemon_path);
        unlink(proxy.sock_path);
        proxy.listen = socket(AF_UNIX, SOCK_SEQPACKET, 0);
        if (proxy.listen < 0) return -errno;
        struct sockaddr_un sa;
        memset(&sa, 0, sizeof(sa));
        sa.sun_family = AF_UNIX;
        snprintf(sa.sun_path, sizeof(sa.sun_path), "%s", proxy.sock_path);
        if (bind(proxy.listen, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
            LOGE("proxy: bind %s failed err=%d", proxy.sock_path, errno);
            close(proxy.listen);
            proxy.listen = -1;
            return -errno;
        }
        if (listen(proxy.listen, 4) < 0) {
            LOGE("proxy: listen failed err=%d", errno);
            close(proxy.listen);
            proxy.listen = -1;
            return -errno;
        }
        LOGI("proxy: listening on %s", proxy.sock_path);
    }
    char cmd[600];
    snprintf(cmd, sizeof(cmd), "%s --connect %s",
             proxy.daemon_path, proxy.sock_path);
    pid_t pid = fork();
    if (pid < 0) return -errno;
    if (pid == 0) {
        char mark[560];
        snprintf(mark, sizeof(mark), "%s.mark", proxy.sock_path);
        int mf = open(mark, O_WRONLY | O_CREAT | O_TRUNC, 0666);
        if (mf >= 0) {
            dprintf(mf, "child pid=%d uid=%d euid=%d\n", getpid(), getuid(), geteuid());
            close(mf);
        }
        /* Capture su's stdout/stderr so we can see why it exits 127
         * (usage message, authorization refusal, exec error...). */
        char slog[560];
        snprintf(slog, sizeof(slog), "%s.spawnlog", proxy.sock_path);
        int sf = open(slog, O_WRONLY | O_CREAT | O_TRUNC, 0666);
        if (sf >= 0) {
            dup2(sf, STDOUT_FILENO);
            dup2(sf, STDERR_FILENO);
            close(sf);
        }
        execl("/system/bin/su", "su", "-c", cmd, (char *)NULL);
        int e = errno;
        mf = open(mark, O_WRONLY | O_APPEND, 0666);
        if (mf >= 0) {
            dprintf(mf, "exec su failed errno=%d\n", e);
            close(mf);
        }
        _exit(127);
    }
    proxy.child = pid;
    LOGI("proxy: spawned pid=%d", pid);

    struct pollfd pfd;
    pfd.fd = proxy.listen;
    pfd.events = POLLIN;
    int pr = poll(&pfd, 1, VDR2_CONNECT_TIMEOUT_MS);
    if (pr <= 0) {
        LOGI("proxy: connect timeout (daemon never connected) pr=%d", pr);
        if (proxy.child > 0) {
            kill(proxy.child, SIGKILL);
            waitpid(proxy.child, NULL, 0);
        }
        proxy_down();
        return -ETIMEDOUT;
    }
    int c = accept(proxy.listen, NULL, NULL);
    if (c < 0) {
        LOGI("proxy: accept failed err=%d", errno);
        if (proxy.child > 0) {
            kill(proxy.child, SIGKILL);
            waitpid(proxy.child, NULL, 0);
        }
        proxy_down();
        return -errno;
    }
    proxy.sock = c;
    LOGI("proxy: daemon connected sock=%d", proxy.sock);
    return 0;
}

/* One request/response round trip. arg/arglen is the ioctl arg block
 * (arglen must match the real struct size), fds ride along on SCM_RIGHTS,
 * outfds receives a fd produced by the ioctl (AUDIO_*, FRAME), -1 if none. */
static int proxy_exchange(unsigned long cmd, void *arg, size_t arglen,
                          int *fds, int nfds, int *outfds)
{
    int retried = 0;
retry:
    if (proxy.sock < 0) {
        int r = proxy_spawn();
        if (r < 0) return r;
    }

    struct vdr2p_msg req;
    req.magic = VDR2P_MAGIC_REQ;
    req.cmd = (uint32_t)cmd;
    req.ret = 0;
    req.arglen = (uint32_t)arglen;
    req.nfds = (uint32_t)nfds;

    char cbuf[CMSG_SPACE(sizeof(int) * VDR2P_MAX_FD)] = {0};
    struct iovec iov[2];
    iov[0].iov_base = &req;
    iov[0].iov_len = sizeof(req);
    iov[1].iov_base = arg;
    iov[1].iov_len = arglen;

    struct msghdr mh = {0};
    mh.msg_iov = iov;
    mh.msg_iovlen = arglen ? 2 : 1;
    if (nfds > 0) {
        mh.msg_control = cbuf;
        mh.msg_controllen = sizeof(cbuf);
        struct cmsghdr *c = CMSG_FIRSTHDR(&mh);
        c->cmsg_level = SOL_SOCKET;
        c->cmsg_type = SCM_RIGHTS;
        c->cmsg_len = CMSG_LEN(sizeof(int) * (size_t)nfds);
        memcpy(CMSG_DATA(c), fds, sizeof(int) * (size_t)nfds);
    }
    if (sendmsg(proxy.sock, &mh, 0) < 0) {
        LOGE("proxy: send failed err=%d", errno);
        proxy_down();
        if (!retried++) goto retry;
        return -EPIPE;
    }

    struct vdr2p_msg rsp;
    char rarg[VDR2P_MAX_ARG];
    char rcbuf[CMSG_SPACE(sizeof(int))] = {0};
    struct iovec riov[2];
    riov[0].iov_base = &rsp;
    riov[0].iov_len = sizeof(rsp);
    riov[1].iov_base = rarg;
    riov[1].iov_len = sizeof(rarg);

    struct msghdr rmh = {0};
    rmh.msg_iov = riov;
    rmh.msg_iovlen = 2;
    rmh.msg_control = rcbuf;
    rmh.msg_controllen = sizeof(rcbuf);

    ssize_t n = recvmsg(proxy.sock, &rmh, 0);
    if (n <= 0) {
        LOGE("proxy: recv failed n=%zd err=%d", n, errno);
        proxy_down();
        if (!retried++) goto retry;
        return -EPIPE;
    }
    if (rsp.magic != VDR2P_MAGIC_RSP) {
        LOGE("proxy: bad rsp magic 0x%x", rsp.magic);
        proxy_down();
        return -EPIPE;
    }
    if (outfds) *outfds = -1;
    if (rsp.nfds) {
        for (struct cmsghdr *c = CMSG_FIRSTHDR(&rmh); c;
             c = CMSG_NXTHDR(&rmh, c)) {
            if (c->cmsg_level == SOL_SOCKET && c->cmsg_type == SCM_RIGHTS) {
                int fd;
                memcpy(&fd, CMSG_DATA(c), sizeof(int));
                if (outfds) *outfds = fd;
                break;
            }
        }
    }
    if (arglen && rsp.arglen) {
        size_t cp = rsp.arglen < arglen ? rsp.arglen : arglen;
        memcpy(arg, rarg, cp);
    }
    return (int)rsp.ret;
}

static int vdr2_open(void)
{
    if (proxy.sock >= 0) return 0;
    int r = proxy_spawn();
    if (r < 0) {
        LOGE("open: proxy spawn failed %d", r);
        return r;
    }
    if (vdr2_bell < 0) {
        vdr2_bell = eventfd(0, EFD_NONBLOCK);
        LOGI("open: bell=%d", vdr2_bell);
        if (vdr2_bell < 0) {
            proxy_down();
            return -errno;
        }
    }
    return 0;
}

static int vdr2_proxy_ioctl(unsigned int req, void *arg, size_t arglen,
                            int *fds, int nfds, int *outfds)
{
    if (vdr2_open() < 0) return -ENODEV;
    if (!vdr2_bell_registered) {
        int ret = proxy_exchange(VDR2_IOC_BEGIN, NULL, 0, NULL, 0, NULL);
        LOGI("begin: ret=%d", ret);
        ret = proxy_exchange(VDR2_IOC_PRESENT, &vdr2_bell, sizeof(int),
                             &vdr2_bell, 1, NULL);
        LOGI("present: ret=%d", ret);
        vdr2_bell_registered = 1;
    }
    return proxy_exchange(req, arg, arglen, fds, nfds, outfds);
}

/* ---- Event helpers ---- */

static int vdr2_send_key(int code, int down)
{
    struct vdr2_ev_io e = { VDR2_EV_KEY, code, down ? 1 : 0, 0, 0 };
    return vdr2_proxy_ioctl(VDR2_IOC_EV, &e, sizeof(e), NULL, 0, NULL);
}

static int vdr2_send_motion(int dx, int dy)
{
    struct vdr2_ev_io e = { VDR2_EV_MOTION, 0, 0, dx, dy };
    return vdr2_proxy_ioctl(VDR2_IOC_EV, &e, sizeof(e), NULL, 0, NULL);
}

static int vdr2_send_btn(int btn, int pressed)
{
    struct vdr2_ev_io e = { VDR2_EV_BUTTON, btn, pressed ? 1 : 0, 0, 0 };
    return vdr2_proxy_ioctl(VDR2_IOC_EV, &e, sizeof(e), NULL, 0, NULL);
}

static int vdr2_send_scroll(int axis, int val)
{
    struct vdr2_ev_io e = { VDR2_EV_SCROLL, axis, val, 0, 0 };
    return vdr2_proxy_ioctl(VDR2_IOC_EV, &e, sizeof(e), NULL, 0, NULL);
}

/* ---- ANativeWindow hidden API ---- */

struct buf_info {
    ANativeWindowBuffer *anb;
    int idx;
    int slot;
};

struct consumer {
    ANativeWindow *win;
    pthread_t thread;
    volatile bool running;

    int buf_count;
    struct buf_info bufs[MAX_BUFS];

    int screen_w;
    int screen_h;

    /* Audio */
    pthread_t play_thr;
    pthread_t cap_thr;
    volatile bool audio_running;
    int play_fd;
    int cap_fd;
    AAudioStream *play_stream;
    AAudioStream *cap_stream;
};

static struct anw_api api;
static bool api_loaded;

static int vdrm_submit(int dmabuf_fd, int slot, unsigned stride)
{
    struct vdr2_reg_io reg = { dmabuf_fd, slot, vdr2_bell, stride };
    int fds[2] = { dmabuf_fd, vdr2_bell };
    LOGI("submit: slot=%d dmabuf_fd=%d bell=%d stride=%u", slot, dmabuf_fd, vdr2_bell, stride);
    int ret = vdr2_proxy_ioctl(VDR2_IOC_REGISTER_BUF, &reg, sizeof(reg),
                               fds, 2, NULL);
    LOGI("submit: ret=%d", ret);
    return ret;
}

__attribute__((unused))
static int vdrm_present(void)
{
    LOGI("present: bell=%d", vdr2_bell);
    int ret = vdr2_proxy_ioctl(VDR2_IOC_PRESENT, &vdr2_bell, sizeof(int),
                               &vdr2_bell, 1, NULL);
    LOGI("present: ret=%d", ret);
    return ret;
}

/* Wait until the kernel rings the bell (a frame flip happened).
 * Blocks up to 5s; returns 0 on ring, -ETIMEDOUT otherwise. */
static int vdr2_wait_fence(void)
{
    struct pollfd pfd = { .fd = vdr2_bell, .events = POLLIN };
    int pr = poll(&pfd, 1, 5000);
    LOGI("wait_fence: bell_fd=%d poll_ret=%d errno=%d", vdr2_bell, pr, errno);
    if (pr <= 0) return pr == 0 ? -ETIMEDOUT : -errno;
    unsigned long long cnt;
    if (read(vdr2_bell, &cnt, sizeof(cnt)) != sizeof(cnt) && errno != EAGAIN)
        return -errno;
    LOGI("wait_fence: drained count=%llu", cnt);
    return 0;
}

/* Ask the KPM which APK slot the last flip rendered into. Returns the slot
 * index, or -1 if no frame is available yet. */
static int vdr2_frame_slot(void)
{
    struct vdr2_frame_io fo;
    memset(&fo, 0, sizeof(fo));
    int frame_fd = -1;
    int ret = vdr2_proxy_ioctl(VDR2_IOC_FRAME, &fo, sizeof(fo),
                               NULL, 0, &frame_fd);
    if (frame_fd >= 0) close(frame_fd);
    LOGI("frame_slot: ret=%d slot=%d seq=%llu", ret, fo.slot, fo.seq);
    if (ret < 0) return -1;
    return fo.slot;
}

static int collect_buffers(struct consumer *c)
{
    ANativeWindow *win = c->win;
    /* 1:1 slot mapping: register exactly VDR2_SLOTS buffers, one per KPM slot.
     * Using more would overwrite earlier slots (found % VDR2_SLOTS) and break
     * the FRAME.slot -> buffer association. */
    int target = VDR2_SLOTS;
    int found = 0;

    LOGI("collecting %d buffers", target);

    for (int attempt = 0; attempt < target * 4 && found < target; attempt++) {
        ANativeWindowBuffer *anb = NULL;
        int fence = -1;
        int dq_ret = api.dequeueBuffer(win, &anb, &fence);
        if (dq_ret != 0 || !anb) {
            LOGE("dequeue failed attempt %d ret=%d anb=%p fence=%d err=%s",
                 attempt, dq_ret, (void*)anb, fence, strerror(-dq_ret));
        if (fence >= 0) {
            struct pollfd pfd = { .fd = fence, .events = POLLIN };
            poll(&pfd, 1, 1000);
            close(fence);
        }
            usleep(16000);
            continue;
        }
        if (fence >= 0) close(fence);

        if (!anb->handle || anb->handle->numFds < 1) {
            LOGE("buffer has no dma-buf fd attempt %d", attempt);
            api.cancelBuffer(win, anb, -1);
            continue;
        }

        int fd = anb->handle->data[0];

        bool is_dup = false;
        for (int i = 0; i < found; i++) {
            if (c->bufs[i].anb == anb) { is_dup = true; break; }
        }

        int submit_fd = -1;
        if (!is_dup) submit_fd = dup(fd);

        api.queueBuffer(win, anb, -1);

        if (is_dup) continue;
        if (submit_fd < 0) continue;

        int slot = found;
        int ret = vdrm_submit(submit_fd, slot, anb->stride);
        close(submit_fd);
        if (ret < 0) {
            LOGE("submit failed fd=%d ret=%d", fd, ret);
            continue;
        }

        c->bufs[found].anb = anb;
        c->bufs[found].idx = found;
        c->bufs[found].slot = slot;
        LOGI("  buf[%d]: anb=%p fd=%d %dx%d stride=%d slot=%d",
             found, (void *)anb, fd, anb->width, anb->height, anb->stride, slot);
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

static void *render_loop(void *arg)
{
    struct consumer *c = arg;
    LOGI("render loop started");
    int loop_count = 0;

    while (c->running) {
        loop_count++;
        /* Wait for the container to finish a flip. */
        int fwait = vdr2_wait_fence();
        if (fwait < 0) {
            LOGI("fence wait failed ret=%d loop=%d", fwait, loop_count);
            /* Even on timeout, query FRAME to trigger vdr2_proxy_ioctl
             * which re-registers the bell if the daemon was restarted. */
            vdr2_frame_slot();
            usleep(16000);
            continue;
        }

        /* Ask the KPM which slot was flipped. */
        int target_slot = vdr2_frame_slot();
        if (target_slot < 0) {
            LOGI("frame slot unavailable loop=%d", loop_count);
            continue;
        }

        LOGI("render frame slot=%d loop=%d", target_slot, loop_count);

        /* Dequeue until we get the buffer that belongs to the flipped slot. */
        for (int retries = 0; retries < VDR2_SLOTS * 2; retries++) {
            ANativeWindowBuffer *anb = NULL;
            int fence = -1;
            int dq_ret = api.dequeueBuffer(c->win, &anb, &fence);
            if (dq_ret != 0 || !anb) {
                LOGE("dequeue failed ret=%d", dq_ret);
                usleep(16000);
                continue;
            }
            if (fence >= 0) {
                struct pollfd pfd = { .fd = fence, .events = POLLIN };
                poll(&pfd, 1, 1000);
                close(fence);
            }
            if (buf_slot(c, anb) == target_slot) {
                api.queueBuffer(c->win, anb, -1);
                break;
            }
            api.cancelBuffer(c->win, anb, -1);
        }
    }

    LOGI("render loop stopped");
    return NULL;
}

/* ---- Audio threads ---- */

static void *audio_play_thread(void *arg)
{
    struct consumer *c = arg;

    int fd = -1;
    int r = vdr2_proxy_ioctl(VDR2_IOC_AUDIO_PLAY, NULL, 0, NULL, 0, &fd);
    LOGI("audio_play: ioctl ret=%d fd=%d", r, fd);
    if (r < 0 || fd < 0) { LOGE("audio play ioctl failed ret=%d", r); return NULL; }
    c->play_fd = fd;

    if (!c->play_stream) {
        LOGE("audio play stream not initialized");
        close(fd);
        c->play_fd = -1;
        return NULL;
    }

    LOGI("audio play thread started");

    aaudio_format_t fmt = AAudioStream_getFormat(c->play_stream);
    int bytes_per_sample = (fmt == AAUDIO_FORMAT_PCM_FLOAT) ? 4 : 2;
    int frame_size = bytes_per_sample * AAudioStream_getChannelCount(c->play_stream);
    short buf[960 * 2];
    int write_count = 0;
    while (c->audio_running) {
        struct pollfd pfd = { .fd = fd, .events = POLLIN };
        int pr = poll(&pfd, 1, 200);
        if (pr <= 0) continue;
        int n = read(fd, buf, sizeof(buf));
        if (n > 0) {
            aaudio_result_t wr = AAudioStream_write(c->play_stream, buf, n / frame_size, 500000000LL);
            if (++write_count % 50 == 1)
                LOGI("audio wrote %d frames (ret=%d)", n / frame_size, wr);
        } else if (n == 0) {
            usleep(10000);
        } else if (n < 0 && errno != EINTR) {
            usleep(10000);
        }
    }

    close(fd);
    c->play_fd = -1;
    LOGI("audio play thread stopped");
    return NULL;
}

static void *audio_cap_thread(void *arg)
{
    struct consumer *c = arg;

    int fd = -1;
    int r = vdr2_proxy_ioctl(VDR2_IOC_AUDIO_CAP, NULL, 0, NULL, 0, &fd);
    if (r < 0 || fd < 0) { LOGE("audio cap ioctl failed ret=%d", r); return NULL; }
    c->cap_fd = fd;

    if (!c->cap_stream) {
        LOGE("audio cap stream not initialized");
        close(fd);
        c->cap_fd = -1;
        return NULL;
    }

    LOGI("audio cap thread started");

    if (AAudioStream_requestStart(c->cap_stream) != AAUDIO_OK) {
        LOGE("audio cap start failed");
        close(fd);
        c->cap_fd = -1;
        return NULL;
    }

    short buf[480 * 1];
    while (c->audio_running) {
        int n = AAudioStream_read(c->cap_stream, buf, sizeof(buf) / sizeof(short), 20000000LL);
        if (n > 0) {
            write(fd, buf, n * sizeof(short));
        } else {
            usleep(10000);
        }
    }

    AAudioStream_requestStop(c->cap_stream);
    close(fd);
    c->cap_fd = -1;
    LOGI("audio cap thread stopped");
    return NULL;
}

/* ---- JNI (static: event helpers don't need handle) ---- */

JNIEXPORT jint JNICALL
Java_com_vdrm_consumer_Native_nativeSendKey(JNIEnv *env, jclass clazz, jint code, jboolean down)
{
    (void)env; (void)clazz;
    return vdr2_send_key((int)code, down ? 1 : 0);
}

JNIEXPORT jint JNICALL
Java_com_vdrm_consumer_Native_nativeSendMotion(JNIEnv *env, jclass clazz, jint dx, jint dy)
{
    (void)env; (void)clazz;
    return vdr2_send_motion((int)dx, (int)dy);
}

JNIEXPORT jint JNICALL
Java_com_vdrm_consumer_Native_nativeSendBtn(JNIEnv *env, jclass clazz, jint btn, jboolean pressed)
{
    (void)env; (void)clazz;
    return vdr2_send_btn((int)btn, pressed ? 1 : 0);
}

JNIEXPORT jint JNICALL
Java_com_vdrm_consumer_Native_nativeSendScroll(JNIEnv *env, jclass clazz, jint axis, jint val)
{
    (void)env; (void)clazz;
    return vdr2_send_scroll((int)axis, (int)val);
}

/* ---- JNI (instance: lifecycle) ---- */

#define STATE(h) ((struct consumer *)(uintptr_t)(h))

static void sighandler_noop(int sig) { (void)sig; }

JNIEXPORT jlong JNICALL
Java_com_vdrm_consumer_Native_nativeCreate(JNIEnv *env, jclass clazz)
{
    (void)env; (void)clazz;
    struct consumer *c = calloc(1, sizeof(*c));
    if (!c) return 0;
    c->play_fd = -1;
    c->cap_fd = -1;

    /* SIGUSR1: empty handler instead of SIG_IGN — SIG_IGN 会让内核吞掉信号，
     * pthread_kill(SIGUSR1) 无法唤醒 present 里阻塞的线程。空 handler 可
     * 让信号正常传递，唤醒线程，handler 返回后线程继续执行。 */
    {
        struct sigaction sa;
        memset(&sa, 0, sizeof(sa));
        sa.sa_handler = sighandler_noop;
        sigaction(SIGUSR1, &sa, NULL);
    }
    /* Socket writes to a dead daemon would kill us via SIGPIPE. */
    signal(SIGPIPE, SIG_IGN);

    LOGI("instance %p created", (void *)c);
    return (jlong)(uintptr_t)c;
}

JNIEXPORT void JNICALL
Java_com_vdrm_consumer_Native_nativeDestroy(JNIEnv *env, jclass clazz, jlong handle)
{
    struct consumer *c = STATE(handle);
    if (!c) return;

    if (c->audio_running) {
        c->audio_running = false;
        pthread_join(c->play_thr, NULL);
        if (c->cap_thr) pthread_join(c->cap_thr, NULL);
    }
    if (c->play_stream) {
        AAudioStream_requestStop(c->play_stream);
        AAudioStream_close(c->play_stream);
        c->play_stream = NULL;
    }
    if (c->cap_stream) {
        AAudioStream_close(c->cap_stream);
        c->cap_stream = NULL;
    }
    if (c->running) {
        c->running = false;
        pthread_kill(c->thread, SIGUSR1);
        pthread_join(c->thread, NULL);
    }
    if (c->win) {
        ANativeWindow_release(c->win);
        c->win = NULL;
    }
    if (proxy.listen >= 0) {
        close(proxy.listen);
        proxy.listen = -1;
    }
    proxy_down();
    LOGI("instance %p destroyed", (void *)c);
    free(c);
}

JNIEXPORT void JNICALL
Java_com_vdrm_consumer_Native_nativeStart(JNIEnv *env, jclass clazz,
                                          jlong handle, jobject surface,
                                          jstring daemonPath)
{
    struct consumer *c = STATE(handle);
    if (!c) return;

    const char *p = daemonPath
        ? (*env)->GetStringUTFChars(env, daemonPath, NULL) : NULL;
    if (p) {
        snprintf(proxy.daemon_path, sizeof(proxy.daemon_path), "%s", p);
        (*env)->ReleaseStringUTFChars(env, daemonPath, p);
        LOGI("daemon path: %s", proxy.daemon_path);
    }
    if (!proxy.daemon_path[0]) {
        LOGE("daemon path not set");
        return;
    }

    if (!api_loaded) {
        if (anw_api_load(&api) < 0) {
            LOGE("failed to load ANativeWindow hidden API");
            return;
        }
        api_loaded = true;
    }

    if (c->running) {
        c->running = false;
        pthread_kill(c->thread, SIGUSR1);
        pthread_join(c->thread, NULL);
    }
    if (c->audio_running) {
        c->audio_running = false;
        pthread_join(c->play_thr, NULL);
        pthread_join(c->cap_thr, NULL);
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
    /* Use VDR2_SLOTS for a 1:1 slot→buffer mapping. The KPM only has
     * VDR2_SLOTS slots, so registering more wastes buffers and breaks
     * the FRAME.slot association. */
    c->buf_count = VDR2_SLOTS;

    /* Sanity check: can we lock/unlock (public API)? */
    ANativeWindow_Buffer buf;
    if (ANativeWindow_lock(c->win, &buf, NULL) == 0) {
        LOGI("lock OK: %dx%d stride=%d fmt=%d bits=%p",
             buf.width, buf.height, buf.stride, buf.format, buf.bits);
        ANativeWindow_unlockAndPost(c->win);
    } else {
        LOGE("lock FAILED - surface may be invalid");
    }

    LOGI("min_ud=%d buf_count=%d", min_ud, c->buf_count);
    int geo_ret = ANativeWindow_setBuffersGeometry(c->win, c->screen_w, c->screen_h,
                                     AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM);
    int sbc_ret = api.setBufferCount(c->win, c->buf_count);
    LOGI("setBuffersGeometry=%d setBufferCount=%d", geo_ret, sbc_ret);

    if (collect_buffers(c) < 0) {
        LOGW("collect_buffers failed, retrying without setBufferCount");
        ANativeWindow_release(c->win);
        c->win = ANativeWindow_fromSurface(env, surface);
        if (!c->win) { LOGE("ANativeWindow_fromSurface failed (retry)"); return; }
        c->screen_w = ANativeWindow_getWidth(c->win);
        c->screen_h = ANativeWindow_getHeight(c->win);
        geo_ret = ANativeWindow_setBuffersGeometry(c->win, c->screen_w, c->screen_h,
                                         AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM);
        LOGI("retry setBuffersGeometry=%d (no setBufferCount)", geo_ret);
        if (collect_buffers(c) < 0) {
            LOGE("collect_buffers failed even without setBufferCount");
            ANativeWindow_release(c->win);
            c->win = NULL;
            return;
        }
    }

    c->running = true;
    pthread_create(&c->thread, NULL, render_loop, c);

    /* Initialize AAudio output + input streams (outside threads, so errors are visible) */
    LOGI("audio init: play_stream=%p cap_stream=%p audio_running=%d",
         (void*)c->play_stream, (void*)c->cap_stream, c->audio_running);
    if (!c->play_stream) {
        AAudioStreamBuilder *bld = NULL;
        if (AAudio_createStreamBuilder(&bld) == AAUDIO_OK && bld) {
            AAudioStreamBuilder_setDirection(bld, AAUDIO_DIRECTION_OUTPUT);
            AAudioStreamBuilder_setFormat(bld, AAUDIO_FORMAT_PCM_I16);
            AAudioStreamBuilder_setSampleRate(bld, 48000);
            AAudioStreamBuilder_setChannelCount(bld, 2);
            AAudioStreamBuilder_setPerformanceMode(bld, AAUDIO_PERFORMANCE_MODE_LOW_LATENCY);
            AAudioStreamBuilder_setSharingMode(bld, AAUDIO_SHARING_MODE_SHARED);
            AAudioStream *s = NULL;
            aaudio_result_t r = AAudioStreamBuilder_openStream(bld, &s);
            AAudioStreamBuilder_delete(bld);
            if (r == AAUDIO_OK && s) {
                AAudioStream_requestStart(s);
                c->play_stream = s;
                LOGI("play stream initialized (%d Hz x%d)", AAudioStream_getSampleRate(s), AAudioStream_getChannelCount(s));
            } else {
                LOGE("AAudio play open failed: %s", AAudio_convertResultToText(r));
            }
        }
    }
    if (!c->cap_stream) {
        AAudioStreamBuilder *bld = NULL;
        if (AAudio_createStreamBuilder(&bld) == AAUDIO_OK && bld) {
            AAudioStreamBuilder_setDirection(bld, AAUDIO_DIRECTION_INPUT);
            AAudioStreamBuilder_setFormat(bld, AAUDIO_FORMAT_PCM_I16);
            AAudioStreamBuilder_setSampleRate(bld, 48000);
            AAudioStreamBuilder_setChannelCount(bld, 1);
            AAudioStreamBuilder_setPerformanceMode(bld, AAUDIO_PERFORMANCE_MODE_LOW_LATENCY);
            AAudioStreamBuilder_setSharingMode(bld, AAUDIO_SHARING_MODE_SHARED);
            AAudioStream *s = NULL;
            aaudio_result_t r = AAudioStreamBuilder_openStream(bld, &s);
            AAudioStreamBuilder_delete(bld);
            if (r == AAUDIO_OK && s) {
                c->cap_stream = s;
                LOGI("cap stream initialized (%d Hz x%d)", AAudioStream_getSampleRate(s), AAudioStream_getChannelCount(s));
            } else {
                LOGE("AAudio cap open failed: %s", AAudio_convertResultToText(r));
            }
        }
    }

    /* Start audio play thread */
    if (c->play_stream && !c->audio_running) {
        c->audio_running = true;
        int ret = pthread_create(&c->play_thr, NULL, audio_play_thread, c);
        if (ret != 0) {
            LOGE("audio play thread create failed: %d", ret);
            c->audio_running = false;
        }
    }

    LOGI("started %dx%d %d bufs", c->screen_w, c->screen_h, c->buf_count);
}

JNIEXPORT void JNICALL
Java_com_vdrm_consumer_Native_nativeStop(JNIEnv *env, jclass clazz, jlong handle)
{
    struct consumer *c = STATE(handle);
    if (!c) return;

    if (c->running) {
        c->running = false;
        pthread_kill(c->thread, SIGUSR1);
        pthread_join(c->thread, NULL);
    }
    if (c->audio_running) {
        c->audio_running = false;
        pthread_join(c->play_thr, NULL);
        if (c->cap_thr) pthread_join(c->cap_thr, NULL);
    }
    if (c->play_stream) {
        AAudioStream_requestStop(c->play_stream);
        AAudioStream_close(c->play_stream);
        c->play_stream = NULL;
    }
    if (c->cap_stream) {
        AAudioStream_close(c->cap_stream);
        c->cap_stream = NULL;
    }
    if (c->win) {
        ANativeWindow_release(c->win);
        c->win = NULL;
    }
    LOGI("stopped");
}

/* ---- JNI (audio start / stop) ---- */

JNIEXPORT void JNICALL
 Java_com_vdrm_consumer_Native_nativeStartAudio(JNIEnv *env, jclass clazz, jlong handle)
{
    struct consumer *c = STATE(handle);
    if (!c) return;

    if (c->audio_running) {
        if (!c->cap_thr) {
            pthread_create(&c->cap_thr, NULL, audio_cap_thread, c);
            LOGI("cap thread started from nativeStartAudio");
        }
        return;
    }

    c->audio_running = true;
    pthread_create(&c->play_thr, NULL, audio_play_thread, c);
    pthread_create(&c->cap_thr, NULL, audio_cap_thread, c);
    LOGI("audio started");
}

JNIEXPORT void JNICALL
 Java_com_vdrm_consumer_Native_nativeStopAudio(JNIEnv *env, jclass clazz, jlong handle)
{
    struct consumer *c = STATE(handle);
    if (!c || !c->audio_running) return;

    c->audio_running = false;
    pthread_join(c->play_thr, NULL);
    if (c->cap_thr) pthread_join(c->cap_thr, NULL);
    LOGI("audio stopped");
}

/* ====================================================================
 * FD import test -> gralloc-render test.
 *
 * System EGL on this device has no EGL_EXT_image_dma_buf_import, so
 * importing an arbitrary dma-buf is impossible. Instead we verify the
 * real VDRM pipeline:
 *
 *   1. dequeue an ANativeWindow buffer (gralloc, displayable by design)
 *   2. send its dma-buf fd to the container (unix socket, SCM_RIGHTS)
 *   3. container GPU renders into that buffer
 *   4. container acks, we queueBuffer -> SurfaceFlinger shows it
 *
 * Screen tells the story:
 *   RED   = container GPU content displayed   (pipeline OK)
 *   BLACK = container render failed           (pipeline broken)
 * ==================================================================== */

#define TEST_SOCK "/data/data/com.vdrm.consumer/vdrm_fd.sock"

struct fdtest_arg {
    ANativeWindow *win;
};

static int fdtest_send_msg_fd(int sock, const char *msg, size_t len, int fd)
{
    char cbuf[CMSG_SPACE(sizeof(int))];
    struct iovec iov = { .iov_base = (void *)msg, .iov_len = len };
    struct msghdr mh = {0};
    mh.msg_iov = &iov;
    mh.msg_iovlen = 1;
    if (fd >= 0) {
        mh.msg_control = cbuf;
        mh.msg_controllen = sizeof(cbuf);
        struct cmsghdr *c = CMSG_FIRSTHDR(&mh);
        c->cmsg_level = SOL_SOCKET;
        c->cmsg_type = SCM_RIGHTS;
        c->cmsg_len = CMSG_LEN(sizeof(int));
        memcpy(CMSG_DATA(c), &fd, sizeof(int));
    }
    return sendmsg(sock, &mh, 0);
}

static void *fdtest_thread(void *arg)
{
    struct fdtest_arg *t = arg;
    ANativeWindow *win = t->win;
    LOGI("test: gralloc-render test started");

    if (anw_api_load(&api) < 0) {
        LOGE("test: failed to load ANativeWindow hidden API");
        return NULL;
    }

    int w = ANativeWindow_getWidth(win);
    int h = ANativeWindow_getHeight(win);
    if (w <= 0) w = 1280;
    if (h <= 0) h = 720;
    ANativeWindow_setBuffersGeometry(win, w, h, AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM);
    api.setBufferCount(win, 3);
    LOGI("test: window %dx%d", w, h);

    unlink(TEST_SOCK);
    int lfd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (lfd < 0) { LOGE("test: socket: %s", strerror(errno)); return NULL; }
    struct sockaddr_un addr = {0};
    addr.sun_family = AF_UNIX;
    strcpy(addr.sun_path, TEST_SOCK);
    chmod(TEST_SOCK, 0777);
    if (bind(lfd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        LOGE("test: bind %s: %s", TEST_SOCK, strerror(errno));
        return NULL;
    }
    listen(lfd, 1);
    LOGI("test: waiting for container on %s", TEST_SOCK);
    int cfd = accept(lfd, NULL, NULL);
    if (cfd < 0) { LOGE("test: accept: %s", strerror(errno)); return NULL; }
    LOGI("test: container connected");

    int frames = 0;
    for (int i = 0; i < 600; i++) {
        ANativeWindowBuffer *anb = NULL;
        int fence = -1;
        int dq_ret = api.dequeueBuffer(win, &anb, &fence);
        if (dq_ret != 0 || !anb) {
            LOGW("test: dequeue failed ret=%d err=%s", dq_ret, strerror(-dq_ret));
            usleep(16000);
            continue;
        }
        if (fence >= 0) close(fence);
        if (!anb->handle || anb->handle->numFds < 1) {
            LOGW("test: buffer has no fd");
            api.cancelBuffer(win, anb, -1);
            continue;
        }

        int fd = dup(anb->handle->data[0]);
        struct {
            char magic[4];
            int w, h, stride;
        } hdr = { {'G','F','D','1'}, anb->width, anb->height, anb->stride };
        int sr = fdtest_send_msg_fd(cfd, (const char *)&hdr, sizeof(hdr), fd);
        close(fd);
        if (sr < 0) { LOGE("test: send fd failed: %s", strerror(errno)); break; }

        char ack[8] = {0};
        ssize_t ar = read(cfd, ack, sizeof(ack));
        if (ar < 0) { LOGE("test: ack failed: %s", strerror(errno)); break; }
        if (ar == 0) { LOGE("test: container closed"); break; }

        if (frames < 5 || frames % 50 == 0)
            LOGI("test: frame %d: buf %dx%d stride=%d ack=%.4s", frames,
                 anb->width, anb->height, anb->stride, ack);
        api.queueBuffer(win, anb, -1);
        frames++;
    }

    if (cfd >= 0) close(cfd);
    close(lfd);
    unlink(TEST_SOCK);
    ANativeWindow_release(win);
    free(t);
    LOGI("test: gralloc-render test done (%d frames)", frames);
    return NULL;
}

JNIEXPORT void JNICALL
Java_com_vdrm_consumer_Native_nativeTestFd(JNIEnv *env, jclass clazz, jobject surface)
{
    (void)clazz;
    ANativeWindow *win = ANativeWindow_fromSurface(env, surface);
    if (!win) { LOGE("test: ANativeWindow_fromSurface failed"); return; }
    struct fdtest_arg *t = malloc(sizeof(*t));
    if (!t) { ANativeWindow_release(win); return; }
    t->win = win;
    pthread_t thr;
    if (pthread_create(&thr, NULL, fdtest_thread, t) != 0) {
        LOGE("test: thread create failed");
        ANativeWindow_release(win);
        free(t);
    }
    pthread_detach(thr);
    LOGI("test: thread started");
}
