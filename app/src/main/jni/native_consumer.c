/*
 * native_consumer.c — VDRM 零拷贝消费者
 *
 * 架构（2026 版，和最初那个 kpatch/KPM 方案无关，注释里别再有 ctl0）：
 *
 *   内核 vdrm.ko 注册一张虚拟 DRM 卡（/dev/dri/card1，连接器 Virtual-3）。
 *   用户态 mkpool 用 gralloc 分配 AHB、把 {fd0, fd1, ints} REGISTER 进内核池
 *   （每档尺寸 6 槽）。合成器（niri 等）在 card1 上 dumb_create 时，驱动直接把
 *   池里的 AHB 发出去 —— 合成器渲染的就是这块显存，零拷贝、可被 DPU 扫描输出。
 *
 *   App 这边：
 *     1. nativeStart(surface)：按 surface 建隐藏 SurfaceControl，向内核注册
 *        eventfd，然后起消费线程；
 *     2. 消费线程 poll(eventfd) → GET_INFO/事件拿到"哪一槽出了新帧" →
 *        latch 到 SurfaceControl（帧就是池槽本身，不再拷贝）；
 *     3. 通过 ASurfaceTransaction_setOnComplete 拿到 SF 的 present 回调，再
 *        调 VDRM_IOCTL_PRESENTED 把"真实上屏时刻"报给驱动 —— flip-done 就是
 *        这么算出来的（见 CHANGES 第三十六项）；
 *     4. 输入：手机触摸/按键经 VDRM_IOCTL_TOUCH / INPUT_BATCH 注入内核虚拟
 *        键鼠；声音走 /dev/vdrm-audio（AAudio）。
 *
 * 不解码、不拷贝、不收放帧：池槽的所有权只在"驱动发出 → App latch → SF
 * present 回调 → RETURN"之间流转。
 */

#include <android/looper.h>
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

#include <time.h>

#include <sys/eventfd.h>
#include <sys/system_properties.h>
#include <sys/ioctl.h>
#include <dirent.h>
#include <sys/un.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>

#include "vdrm_audio.h"

#include <android/hardware_buffer.h>
#include <android/choreographer.h>
#include <android/looper.h>
#include <android/surface_control.h>

/* A/B：把 KPM 池里的东西变回 AHB（见 ahb_forge.c）*/
extern AHardwareBuffer *vdrm_ahb_rebuild(int fd0, int fd1, const uint32_t *ints,
                                         uint32_t num_ints, uint32_t w, uint32_t h,
                                         uint32_t stride_bytes, uint32_t hal_fmt,
                                         uint64_t usage);

static ASurfaceControl *g_sc;   /* 从窗口建的 SurfaceControl（显示出口）*/
static jobject g_last_surface;  /* 上次建 SC 用的 Surface（判断是否同一块）*/
static AHardwareBuffer *g_last_ahb;   /* 最后一帧（切前台重贴用）*/
static uint32_t g_last_slot, g_last_w, g_last_h;
/* 当前"正在屏上"的那一槽。这个槽绝不能因为"SF 迟迟不回调"就被强制归还：
 * 屏上还显示着它，一还回池，驱动就可能把同一片内存再发给合成器 —— 合成器
 * 往别处画、屏上是这块，或者两个 BO 指向同一片内存，表现就是闪一下/花一块。*/
static uint32_t g_cur_slot;
static void on_buf_release(void *context, int release_fd);
static pthread_mutex_t g_display_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t g_geometry_lock = PTHREAD_MUTEX_INITIALIZER;
struct submission {
    struct submission *next;
    AHardwareBuffer *ahb;
    uint32_t slot;
    int fence_fd;
    uint64_t token;        /* 给 release 回调的身份；0 = 空闲块 */
    uint64_t created_ms;   /* 建的时间，watchdog 用 */
};
/* submission 对象常驻复用：每帧 calloc/free 没必要，槽位固定后直接拿。 */
#define SUBMISSION_CACHE_MAX 32
static struct submission *g_submissions;
static struct submission *g_submission_free;
static struct submission g_submission_cache[SUBMISSION_CACHE_MAX];
static bool g_submission_cache_ready;
static atomic_size_t g_submissions_live;
/* release 回调可能迟到（超时被强制 RETURN 之后才来），所以不能让它拿着
 * struct submission * 直接写 —— 那个块早被复用给别的帧了。回调只传 token，
 * 按 token 在活动表里找；找不到就说明是过期回调，只把 fd 关掉。*/
static atomic_uint_fast64_t g_sub_token;

/* ---- 延迟预算：把"内核推帧 → App 读到 → apply → latch → present → RETURN"
 * 六段都量出来。以前只有中间两段（apply2latch / latch2present），前面
 * "commit→读到" 和后面的 "present→RETURN" 是黑的，没法判断该优化哪一段。*/
static unsigned long long g_p2r_last_us, g_p2r_max_us;   /* push(内核) → read */
static unsigned long long g_r2a_last_us, g_r2a_max_us;   /* read → apply */
static unsigned long long g_p2t_last_us, g_p2t_max_us;   /* present → RETURN */
static uint64_t g_ev_read_ns;                             /* 最近一次读到帧事件的时刻 */
static uint64_t g_present_ns_arr[64];                     /* 按 slot 记 present 时刻 */

static uint64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)(ts.tv_nsec / 1000000);
}

static uint64_t mono_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

/* ---- 链路延迟分解（诊断）----
 *
 * 端到端 30ms 到底花在哪，拆成两段量：
 *
 *   apply -> latch    App 提交事务 → SF 锁定它。**这段是唯一我们能优化的**：
 *                     如果落在 SF 本帧 deadline 之后，就白等一个 vsync。
 *   latch -> present  SF 锁定 → present fence signal（合成 + 扫描出）。
 *
 * 第三段（flip-done → 合成器下一次 commit）由内核的 present_lat_us 和
 * fence_* 覆盖，不用在这里重复。
 *
 * 按 slot 记：合成器一次只会在一个槽上等 present（flip-done 门控保证了
 * 一个槽不会有两个在飞帧），所以 &31 就够。*/
#define LAT_SLOTS 32
static uint64_t g_apply_ns[LAT_SLOTS];
static uint64_t g_latch_ns[LAT_SLOTS];
static unsigned long long g_a2l_last_us, g_a2l_max_us;
static unsigned long long g_l2p_last_us, g_l2p_max_us;
static unsigned long long g_lat_rejects;   /* 时钟域不一致时的兜底计数 */

/* ---- 卡顿诊断：内核推来相邻两帧的间隔，以及一次 latch 的耗时 ----
 *
 * 链路是串行的：niri commit → 内核推帧事件 → App latch → SF 合成 → present
 * → flip-done → niri 下一帧。所以"偶尔卡一下"要么是事件来晚了（上游慢），
 * 要么是 App 自己在这段里停住了。这两个数就是用来分辨的：
 *   gap 大 + latch 正常  → 上游（合成器/内核推帧）
 *   gap 大 + latch 也大  → 我们自己的 latch 卡住了
 */
static unsigned long long g_ev_gap_max_ms;
static unsigned long long g_ev_gap_n40, g_ev_gap_n80;
static unsigned long long g_latch_max_us;
static uint64_t g_last_ev_ns;

/* Java 的 mode 回调：内核在 commit / pipe_enable 时 signal eventfd，渲染线程
 * 被唤醒后调用它。以前 Java 是 500ms 轮询 GET_INFO，内容切完窗口才转；
 * 现在 commit 就通知，窗口旋转动画能和合成器换档重叠。*/
static JavaVM *g_jvm;
static jclass g_native_class;
static jmethodID g_on_mode_changed;

static void notify_mode_changed(void)
{
    JNIEnv *env;
    int detach = 0;

    if (!g_jvm || !g_native_class || !g_on_mode_changed)
        return;
    if ((*g_jvm)->GetEnv(g_jvm, (void **)&env, JNI_VERSION_1_6) != JNI_OK) {
        if ((*g_jvm)->AttachCurrentThread(g_jvm, &env, NULL) != JNI_OK)
            return;
        detach = 1;
    }
    (*env)->CallStaticVoidMethod(env, g_native_class, g_on_mode_changed);
    if ((*env)->ExceptionCheck(env))
        (*env)->ExceptionClear(env);
    if (detach)
        (*g_jvm)->DetachCurrentThread(g_jvm);
}


/* in-flight 上限：debug.vdrm.inflight 可调（1..6），默认 4。*/
/* 调试页设的 in-flight 上限；-1 = 用属性/默认值 */
static volatile int g_inflight_override = -1;
/* App 侧节拍模式：0=立即贴；1=等上一帧 present；2=允许一帧在飞。
 * 由调试页的 nativeSetPace 或 debug.vdrm.pace 设置；内核 pace 始终 0。*/

static int max_inflight(void)
{
    static int cached = -1;
    char v[PROP_VALUE_MAX];

    if (g_inflight_override >= 1 && g_inflight_override <= 6)
        return g_inflight_override;
    if (cached < 0) {
        int n = 4;

        if (__system_property_get("debug.vdrm.inflight", v) > 0) {
            int t = atoi(v);
            if (t >= 1 && t <= 6)
                n = t;
        }
        cached = n;
    }
    return cached;
}

/* 回调迟迟不来：超过这个时间就强制回收。
 *
 * 这个值不能小。SF 的 release 回调挂在我们上屏的那一帧被真正替换掉之后，
 * 屏幕静止、或 SF 这一轮没合成这个图层时，回调会晚很多 —— 300ms 时实测
 * 每隔两三秒就会"强制 RETURN"一次，而那块 buffer 其实还在扫描输出：
 * 驱动随即又把它发给合成器，合成器只重画脏区，于是屏幕上留下大片灰块
 * （上一帧的残留）。现在按 fence 超时同档（2s），只当真正的卡死保险。*/
#define SUB_STUCK_MS 2000
#define SUB_FENCE_TIMEOUT_MS 2000
/* 几何/透明度是图层属性，只在变化时下发；调用方都持 g_geometry_lock。 */
static ASurfaceControl *g_geom_sc;
static uint32_t g_geom_mw, g_geom_mh, g_geom_ww, g_geom_wh;
static int g_geom_rot;

/* 图层旋转用的 transform（NATIVE_WINDOW_TRANSFORM_*，见 native_window.h）。*/
#define VDRM_ROT_NONE   0
#define VDRM_ROT_90     4          /* NATIVE_WINDOW_TRANSFORM_ROT_90 */
#define VDRM_ROT_270    7          /* NATIVE_WINDOW_TRANSFORM_ROT_270 */

/* 这块 buffer 上屏时该转多少度。
 *
 * 两件事各管一半：
 *   - App 的窗口方向由 Java 按模块的"配置 mode"强制（竖配置 → 竖窗口）；
 *   - 但窗口方向不一定就跟得上配置（系统限制、分屏、旧 APK 还锁着 landscape），
 *     这时如果只做等比 letterbox，竖配置就缩成中间一条。所以这里再按
 *     "buffer 方向 ≠ 窗口方向"补一次 90° 旋转，让画面铺满。
 * 两边方向一致时 rot = 0，等于什么都不做。
 *
 * 运行时可干预（不用重编）：
 *     setprop debug.vdrm.rotate 0 | 90 | 270    强制；默认 auto。*/
static int present_rot(uint32_t mw, uint32_t mh, uint32_t ww, uint32_t wh)
{
    static int forced = -1;
    char v[PROP_VALUE_MAX];

    if (forced < 0) {
        /* 属性只在**第一次**用到时读一次：运行中改了要重启 App 才生效，
         * 但够用来试方向了。认 auto / 0 / 90 / 270，其余按 auto。*/
        if (__system_property_get("debug.vdrm.rotate", v) > 0) {
            if (v[0] == 'a' || v[0] == 'A') {
                forced = -1;
            } else {
                int n = atoi(v);
                forced = (n == 0) ? VDRM_ROT_NONE
                       : (n == 90) ? VDRM_ROT_90
                       : (n == 270) ? VDRM_ROT_270 : -1;
            }
        } else {
            forced = -1;
        }
    }
    if (forced >= 0)
        return forced;

    /* 默认【不转图层】，只做等比 letterbox。
     *
     * 以前这里按"mode 方向 ≠ 窗口方向"补 90° 去铺满窗口，结果在窗口和合成器
     * 没对齐的窗口期（系统还没转完屏、或小窗根本不让转）会把画面转成竖排文字
     * —— 用户看到的就是"旋转错了"。
     *
     * 现在的约定：方向由窗口负责（App 按 GET_INFO 的真实 active 尺寸申请转屏），
     * 图层永远不自己转。窗口没转过来的时候，看到的是一张摆正的、带黑边的画面，
     * 而不是一张转倒的画面。要强制试别的方向仍可用 debug.vdrm.rotate=90/270。*/
    (void)mw; (void)mh; (void)ww; (void)wh;
    return VDRM_ROT_NONE;
}

/* 一块 buffer 贴进窗口时的摆放：旋转 → 等比缩放 → 居中。
 * 旋转后 buffer 的显示尺寸变成 (mh, mw)，再按比例塞进窗口。*/
static void present_layout(uint32_t mw, uint32_t mh, uint32_t ww, uint32_t wh,
                           int *rot, int32_t *dw, int32_t *dh,
                           int32_t *dx, int32_t *dy)
{
    double pw, ph, s, s2;

    *rot = present_rot(mw, mh, ww, wh);
    pw = (*rot == VDRM_ROT_NONE) ? (double)mw : (double)mh;
    ph = (*rot == VDRM_ROT_NONE) ? (double)mh : (double)mw;
    if (pw < 1.0) pw = 1.0;
    if (ph < 1.0) ph = 1.0;

    s  = (double)ww / pw;
    s2 = (double)wh / ph;
    if (s2 < s) s = s2;
    if (s <= 0.0) s = 1.0;
    *dw = (int32_t)(pw * s + 0.5);
    *dh = (int32_t)(ph * s + 0.5);
    if (*dw < 1) *dw = 1;
    if (*dh < 1) *dh = 1;
    *dx = ((int32_t)ww - *dw) / 2;
    *dy = ((int32_t)wh - *dh) / 2;
}
static ASurfaceControl *g_opaque_sc;
static ASurfaceControl *g_fr_sc;   /* 已经下发过 frame rate 的 SC */
/* 最近一次真正上屏的 buffer 尺寸。切分辨率时模块的 mode 已经变了、新 buffer
 * 还没来，这时如果按新 mode 算 letterbox，旧画面就会被横向拉长/压扁 ——
 * 所以几何一律按"正在显示的那块 buffer"算。*/
static uint32_t g_last_buf_w, g_last_buf_h;
static int g_release_wake_fd = -1;
/* 单个常驻 release worker：不再每帧 pthread_create。*/
static int g_release_worker_fd = -1;
static bool g_release_worker_ok;
static pthread_once_t g_release_once = PTHREAD_ONCE_INIT;
static struct submission *submission_create(AHardwareBuffer *ahb, uint32_t slot);
static void submission_finish(struct submission *s);
static void submission_abandon(struct submission *s);
static void submission_release(struct submission *s, bool give_back);
static size_t submission_count(void);

static void sc_geometry(uint32_t mw, uint32_t mh, uint32_t ww, uint32_t wh);
static void vdrm_return(int id);

/* Java onPause/onResume 的显式后台信号。*/
static atomic_bool g_paused;

static void on_present(void *context, ASurfaceTransactionStats *stats);

static void vdrm_presented(int id);


static void maybe_request_pool(void);   /* 池按需创建（定义在后面）*/

/* 「等 SF release 才还槽」的表（定义在后面）*/
static void held_add(uint64_t token, uint32_t slot);
static bool held_take_return(uint64_t token);
static void held_expire(void);
static void held_expire_force(bool force);

/* 把一块 AHB 贴到 SC 上（可反复调用，用于重贴最后一帧）*/
static void sc_set_geometry(ASurfaceTransaction *t, uint32_t mw, uint32_t mh,
                            uint32_t ww, uint32_t wh)
{
    /* src = KPM 模式(buffer)；dst = 等比缩放(letterbox)后居中放进 Android 窗口。
     * 保持宽高比不拉伸；mode 与屏比例不一致时四周留黑边。*/
    ARect src = {0, 0, (int32_t)mw, (int32_t)mh};
    ARect dst;
    int rot;

    if (!g_sc)
        return;

    /* 让 SF 把这个 layer 的 frame rate 拉到 120：display 的 latch 网格从
     * 16.7ms 变 8.3ms，60fps 内容的最坏等待减半。每个 SC 只下发一次。 */
    if (g_fr_sc != g_sc) {
        /* ASurfaceTransaction_setFrameRate 是 API 31 才有的；minSdk 29 的
         * 设备上直接调用会 "cannot locate symbol" 崩在首帧。*/
        typedef void (*set_fr_fn)(ASurfaceTransaction *, ASurfaceControl *,
                                  float, int32_t);
        static set_fr_fn set_fr;
        static bool tried;
        if (!tried) {
            tried = true;
            set_fr = (set_fr_fn)dlsym(RTLD_DEFAULT,
                                      "ASurfaceTransaction_setFrameRate");
        }
        /* 不再投 120 的票。
         *
         * 这台是 LTPO 屏，SF 的 VRR 策略会在 60/90/120 之间不断切模式；每切一次
         * 面板都要重配 gamma/boost，暗画面（容器桌面就是这样）上会看到闪一下。
         * App 硬投 120 等于不停把策略往 120 拉，和其他窗口/空闲计时器打架，
         * 反而制造更多模式切换。让 SF 按内容实际的帧节奏自己选。*/
        /* 投票策略可调：debug.vdrm.frame_rate = 0 表示不投（历史默认），
         * >0 表示恒定投这个帧率（FIXED_SOURCE）。
         *
         * 以前不投是因为 LTPO 上 60/90/120 来回切会闪；现在有了锁相环，
         * 只要面板稳定在同一个频率，tick 就能锁住它 —— 值得重新试
         * "稳定投 120" 这一档。*/
        {
            static int fr_cached = -1;
            int fr = 0;   /* 默认不投票（保守）；调试页可改成恒定帧率 */

            if (fr_cached < 0) {
                char v[PROP_VALUE_MAX];

                if (__system_property_get("debug.vdrm.frame_rate", v) > 0) {
                    int t = atoi(v);

                    if (t >= 0 && t <= 240)
                        fr = t;
                }
                fr_cached = fr;
            }
            if (set_fr && fr > 0)
                set_fr(t, g_sc, (float)fr,
                       ANATIVEWINDOW_FRAME_RATE_COMPATIBILITY_FIXED_SOURCE);
            else if (set_fr)
                set_fr(t, g_sc, 0.0f,
                       ANATIVEWINDOW_FRAME_RATE_COMPATIBILITY_DEFAULT);
        }
        g_fr_sc = g_sc;
    }

    {
        int32_t dw, dh, dx, dy;

        present_layout(mw, mh, ww, wh, &rot, &dw, &dh, &dx, &dy);
        dst.left = dx; dst.top = dy;
        dst.right = dx + dw; dst.bottom = dy + dh;
    }

    /* 层实际是 XRGB（Alpha 无意义）；不标 opaque 时 SF 会按
     * PREMULTIPLIED 混合。只在新 SurfaceControl 上设一次。 */
    if (g_opaque_sc != g_sc) {
        ASurfaceTransaction_setBufferTransparency(
            t, g_sc, ASURFACE_TRANSACTION_TRANSPARENCY_OPAQUE);
        g_opaque_sc = g_sc;
    }

    ASurfaceTransaction_setVisibility(t, g_sc,
                                      ASURFACE_TRANSACTION_VISIBILITY_SHOW);

    /* 几何只在窗口/模式/旋转变化时变；每帧重复 setGeometry 没有意义。 */
    if (g_geom_sc == g_sc && g_geom_mw == mw && g_geom_mh == mh &&
        g_geom_ww == ww && g_geom_wh == wh && g_geom_rot == rot)
        return;

    ASurfaceTransaction_setGeometry(t, g_sc, &src, &dst, rot);
    __android_log_print(ANDROID_LOG_INFO, "VDRM",
                        "geom mode=%ux%u win=%ux%u rot=%d dst=%d,%d %dx%d",
                        mw, mh, ww, wh, rot, dst.left, dst.top,
                        dst.right - dst.left, dst.bottom - dst.top);
    g_geom_sc = g_sc;
    g_geom_mw = mw; g_geom_mh = mh;
    g_geom_ww = ww; g_geom_wh = wh;
    g_geom_rot = rot;
}

/* 先把图层藏起来，等新尺寸的帧到了 sc_latch 会再 SHOW。
 *
 * 为什么需要：切分辨率时合成器不一定立刻重绘（空闲桌面就一帧都不出），
 * 而窗口已经跟着新尺寸转了。这时候如果继续显示"上一档的旧帧 + 新窗口几何"，
 * 看到的就是一张被拉伸/转错方向的画面（实测像"反向竖屏"）。宁可黑一下。*/
static void sc_blank(void)
{
    ASurfaceTransaction *t;

    if (!g_sc)
        return;
    t = ASurfaceTransaction_create();
    ASurfaceTransaction_setVisibility(t, g_sc,
                                      ASURFACE_TRANSACTION_VISIBILITY_HIDE);
    ASurfaceTransaction_apply(t);
    ASurfaceTransaction_delete(t);
}

static void sc_geometry(uint32_t mw, uint32_t mh, uint32_t ww, uint32_t wh)
{
    /* 优先用"正在显示的 buffer"的真实尺寸；只有还没贴过任何 frame 时才退回
     * 传入的 mode。否则切分辨率时会出现短暂的横向拉伸（新模式 + 旧 buffer）。*/
    uint32_t bw = g_last_buf_w ? g_last_buf_w : mw;
    uint32_t bh = g_last_buf_h ? g_last_buf_h : mh;

    if (!g_sc) return;
    ASurfaceTransaction *t = ASurfaceTransaction_create();
    sc_set_geometry(t, bw, bh, ww, wh);
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
    struct submission *s = submission_create(ahb, slot);
    if (!s)
        return false;

    {
        /* 几何按这块 buffer 的真实宽高算，和模块的 mode 解耦 */
        AHardwareBuffer_Desc d;

        memset(&d, 0, sizeof(d));
        AHardwareBuffer_describe(ahb, &d);
        g_last_buf_w = d.width ? d.width : mw;
        g_last_buf_h = d.height ? d.height : mh;
    }

    ASurfaceTransaction *t = ASurfaceTransaction_create();
    if (!set_buffer_release) {
        /* 没有带 release 回调的入口（API<36）：退回公开的 setBuffer。
         * 拿不到"SF 用完了"的信号，只能立刻归还槽——可能有一帧的撕裂，
         * 但总比不出图强。只提示一次。*/
        static bool warned;
        if (!warned) {
            warned = true;
            __android_log_print(ANDROID_LOG_WARN, "VDRM",
                "setBufferWithRelease missing; fallback to setBuffer");
        }
        ASurfaceTransaction_setBuffer(t, g_sc, ahb, -1);
        sc_set_geometry(t, g_last_buf_w, g_last_buf_h, ww, wh);
        ASurfaceTransaction_setOnComplete(t, (void *)(uintptr_t)slot,
                                          on_present);
        g_apply_ns[slot & (LAT_SLOTS - 1)] = mono_ns();   /* 延迟分解起点 */
        ASurfaceTransaction_apply(t);
        ASurfaceTransaction_delete(t);
        submission_finish(s);
        return true;
    }
    set_buffer_release(t, g_sc, ahb, -1, (void *)(uintptr_t)s->token, on_buf_release);
    sc_set_geometry(t, g_last_buf_w, g_last_buf_h, ww, wh);
    ASurfaceTransaction_setOnComplete(t, (void *)(uintptr_t)slot, on_present);
    /* setDesiredPresentTime("马上送我") 试过，没用：apply -> latch 仍是
     * 14ms、总帧率仍是 30fps —— SF 不按这个 hint 提前。留着只是每帧多一次
     * 调用，删掉。（结论记在 CHANGES 第 47 项。）*/
    g_apply_ns[slot & (LAT_SLOTS - 1)] = mono_ns();       /* 延迟分解起点 */
    ASurfaceTransaction_apply(t);
    ASurfaceTransaction_delete(t);
    {   /* latch 本身花了多久（apply 是 binder 进 SF，正常情况下几十微秒）*/
        unsigned long long dt =
            (mono_ns() - g_apply_ns[slot & (LAT_SLOTS - 1)]) / 1000;

        if (dt > g_latch_max_us)
            g_latch_max_us = dt;
    }

    /* 记下这一帧：Surface 重建（切后台回来、息屏再亮）时新图层是空的，
     * 要马上把它贴回去，否则那块区域在新合成器推第一帧之前是黑的。*/
    g_last_ahb  = ahb;
    g_last_slot = slot;
    g_cur_slot  = slot;
    g_last_w    = g_last_buf_w;
    g_last_h    = g_last_buf_h;
    return true;
}

/* 新 SurfaceControl 建好后，把"最后上过屏的那帧"立刻贴回去。
 * 用 setBuffer（不带 release 回调）：这只是把已经在池里的内容重新摆上去，
 * 不涉及池的归属，下一帧来了自然会被替换。*/
static void sc_show_last(uint32_t ww, uint32_t wh)
{
    ASurfaceTransaction *t;

    if (!g_sc || !g_last_ahb || !g_last_w || !g_last_h)
        return;

    t = ASurfaceTransaction_create();
    ASurfaceTransaction_setBuffer(t, g_sc, g_last_ahb, -1);
    sc_set_geometry(t, g_last_w, g_last_h, ww, wh);
    ASurfaceTransaction_apply(t);
    ASurfaceTransaction_delete(t);
    __android_log_print(ANDROID_LOG_INFO, "VDRM",
                        "re-latch last frame %ux%u (slot %u) into new SC",
                        g_last_w, g_last_h, g_last_slot);
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
#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

/* 池槽上限（= KPM VDRM_POOL_MAX）。必须 >= KPM 池可能出现的 entry 数，
 * 否则池里攒了多批时 App 只 forge 前 N 个，而合成器可能挑到第 N+1 个 ->
 * unknown frame slot -> 黑屏。forge 的是同一批 pool buffer（共享内存，
 * 不额外占显存），放宽到 16 几乎不花代价。*/
#define VDR2_BUFS 16
#define VDRM_DEV "/dev/vdrm"

/* ======================== Step 0: SurfaceControl 直贴探针 ========================
 * 目的：验证在 App 自己的进程里，能不能 (a) 从自己的窗口建一个 ASurfaceControl，
 * (b) 把一块 AHardwareBuffer 贴上去显示。若行，hybrid 就能在 App 内完成。 */
static ASurfaceControl *g_sc;
/* ---- /dev/vdrm 接口（与内核 vdrm_ko.c 一致）---- */
struct vdrm_info {
    uint32_t abi_version;
    uint32_t width, height, format, pitch;
    uint32_t pool_capacity;
    uint32_t flags;     /* 能力位图 VDRM_CAP_*；0 = 老模块，没有能力位图 */
    uint32_t reserved;
};
/* 和能力位图对应：不要拿 abi_version 比大小，问"你支持这个吗"。
 * 老模块（flags==0）什么都不支持，只能按 ABI 1 的老行为跑。*/
#define VDRM_CAP_MODE_RUNTIME     (1u << 0)
#define VDRM_CAP_REFRESH_RUNTIME  (1u << 1)
#define VDRM_CAP_DPI_RUNTIME      (1u << 2)
#define VDRM_CAP_EVENTFD          (1u << 3)
#define VDRM_CAP_PRESENTED        (1u << 4)
#define VDRM_CAP_POOL             (1u << 5)
#define VDRM_CAP_INPUT_BATCH      (1u << 6)
#define VDRM_CAP_TOUCH            (1u << 7)
#define VDRM_CAP_POOL_HANDLE_GONE (1u << 8)
#define VDRM_CAP_READY            (1u << 9)
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
/* 池按需创建：内核请求的目标尺寸 + 代次（ioctl，不读 sysfs）*/
struct vdrm_want {
    uint32_t gen;
    uint32_t width;
    uint32_t height;
};
#define VDRM_IOCTL_WANT        VDRM_IOC(2, 'V', 0x0A, sizeof(struct vdrm_want))
/* 假 vblank 的观测/调相（内核 vdrm_ko.c 的 TICK_INFO / TICK_PHASE）*/
struct vdrm_tick_info {
    int64_t  last_tick_ns;
    uint64_t period_ns;
    int64_t  phase_ns;
    uint32_t running;
    uint32_t reserved;
};
#define VDRM_IOCTL_TICK_INFO   VDRM_IOC(2, 'V', 0x0c, sizeof(struct vdrm_tick_info))
#define VDRM_IOCTL_TICK_PHASE  VDRM_IOC(1, 'V', 0x0d, sizeof(int64_t))

/* 锁相环：把内核的假 vblank 锁到面板 vsync 上。
 *
 * 目标不是"和面板同相"，而是让我们的 tick 比面板 vsync **早一点** ——
 * 合成器 commit → App apply 要花 ~0.1ms，SF 还有自己的处理时间；tick 落在
 * deadline 之后，apply2latch 就整帧白等（实测 9.6ms vs 2ms 两档）。
 * 增益取 0.3，避免一次调过头；周期里做 1/2 环绕。*/
static volatile int g_pll_enabled;     /* 默认关：调试页里手动开 */
static uint64_t g_panel_vsync_ns;      /* 最近一次 Choreographer 回调时刻 */
static int64_t  g_pll_phase_ns;
static int64_t  g_pll_delta_ns;        /* 最近一次算出的相位误差（诊断）*/
static uint64_t g_pll_last_ms;
/* 模块的"配置"（mode 参数）：App 用它决定自己窗口该横着还是竖着。
 * 和 GET_INFO 的 width/height（合成器已经切过去的尺寸）区分开：方向必须看配置，
 * 否则会跟着合成器慢半拍。*/
struct vdrm_conf {
    uint32_t width;
    uint32_t height;
    uint32_t screen_w;
    uint32_t screen_h;
    uint32_t mode_gen;
    uint32_t commit_gen;   /* 每次 commit +1；App 靠它提前转窗口 */
};
#define VDRM_IOCTL_GET_CONF    VDRM_IOC(2, 'V', 0x0d, sizeof(struct vdrm_conf))
#define VDRM_IOCTL_RETURN      VDRM_IOC(1, 'V', 0x02, sizeof(struct vdrm_handle_req))
#define VDRM_IOCTL_SET_EVENTFD VDRM_IOC(1, 'V', 0x04, sizeof(int))
#define VDRM_IOCTL_INPUT       VDRM_IOC(1, 'V', 0x06, sizeof(struct vdrm_input_event))
#define VDRM_INPUT_BATCH_MAX 16
struct vdrm_input_batch {
    uint32_t count;
    uint32_t flags;
    struct vdrm_input_event events[VDRM_INPUT_BATCH_MAX];
};
#define VDRM_IOCTL_INPUT_BATCH VDRM_IOC(1, 'V', 0x0b, sizeof(struct vdrm_input_batch))

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
/* 【ABI 常量】GET_POOL 的 ioctl 号里编码了这个结构的大小，必须和内核的
 * VDRM_POOL_ABI_MAX 一致；不能跟着内部池上限变（改错的话 forge_pool 拿不到
 * 任何 buffer，屏幕全黑）。*/
#define VDRM_POOL_MAX 32
struct vdrm_pool_list {
    uint32_t count;
    uint32_t reserved;
    struct vdrm_pool_info items[VDRM_POOL_MAX];
};
#define VDRM_IOCTL_GET_POOL    VDRM_IOC(3, 'V', 0x07, sizeof(struct vdrm_pool_list))

/* 直接触摸（必须与内核 struct vdrm_touch_frame 逐字节一致）。
 * 每帧发送当前所有活动触点；内核负责自动 lift、BTN_TOUCH 和唯一 SYN。*/
#define VDRM_TOUCH_MAX        10
#define VDRM_TOUCH_COORD_MAX  4095
struct vdrm_touch_point {
    int32_t slot;
    int32_t tracking;
    int32_t x, y;
    int32_t pressure;
};
struct vdrm_touch_frame {
    uint32_t count;
    uint32_t flags;
    struct vdrm_touch_point points[VDRM_TOUCH_MAX];
};
#define VDRM_IOCTL_TOUCH       VDRM_IOC(1, 'V', 0x09, sizeof(struct vdrm_touch_frame))

/* ---- 帧事件：/dev/vdrm 的 read() 直接返回这个（必须与 KPM
 *      struct vdrm_frame_event 一致）---- */
struct vdrm_frame_event {
    uint32_t id;            /* 池 id == slot */
    uint32_t sequence;
    uint64_t timestamp_ns;
    uint64_t user_data;
};

/* ---- per-buffer bookkeeping ---- */
struct buf_info {
    AHardwareBuffer *ahb;   /* 从池槽 forge 出来的 AHB（SC 直贴）*/
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
    uint32_t last_seq;

    /* Audio: /dev/vdrm-audio ring -> AAudio
     * 默认只打开 ring + monitor；容器真开 PCM 时才启 AAudio。
     * 老模块没有 GET_STATE/eventfd 时 audio_legacy=1，退回常开。*/
    pthread_t play_thr;
    pthread_t cap_thr;
    pthread_t audio_mon_thr;
    volatile bool audio_running;
    volatile bool audio_mon_running;
    bool audio_legacy;
    int audio_fd;
    int audio_ev_fd;
    void *audio_map;
    size_t audio_map_len;
    AAudioStream *play_stream;
    AAudioStream *cap_stream;
    /* 采集：/dev/vdrm-audio-in 的环，App 往里写麦克风数据 */
    volatile bool cap_running;
    int cap_fd;
    void *cap_map;
    size_t cap_map_len;
};
/* 渲染循环里用：模块侧运行时改了分辨率时重建池。*/
static void maybe_rebuild_pool(struct consumer *c);
static void touch_update_geometry(int mw, int mh, int ww, int wh);
static void sc_blank(void);


/* ======================== /dev/vdrm 通道 ========================
 * 新 KPM 用 misc 设备 /dev/vdrm，不再走 `kpatch kpm ctl0`。
 * 注：普通 App 进程可能无权限打开 /dev/vdrm（DAC + SELinux），
 * 所以启动时先用 su 放宽一下权限。
 */
static int g_vdrm_fd = -1;
/* release worker 用的常驻 fd：避免每帧 dup/close。 */
static int g_return_fd = -1;

/* 把能力位图翻成一句人话：一眼看出这版模块支持哪些面。
 * flags 的位只增不改（内核侧 VDRM_CAP_*），用户态遇到不认识的位就忽略。*/
static const char *vdrm_caps_str(uint32_t caps)
{
    static const struct { uint32_t bit; const char *name; } tab[] = {
        { VDRM_CAP_MODE_RUNTIME,     "mode" },
        { VDRM_CAP_REFRESH_RUNTIME,  "refresh" },
        { VDRM_CAP_DPI_RUNTIME,      "dpi" },
        { VDRM_CAP_EVENTFD,          "eventfd" },
        { VDRM_CAP_PRESENTED,        "presented" },
        { VDRM_CAP_POOL,             "pool" },
        { VDRM_CAP_INPUT_BATCH,      "batch" },
        { VDRM_CAP_TOUCH,            "touch" },
        { VDRM_CAP_POOL_HANDLE_GONE, "handle_gone" },
        { VDRM_CAP_READY,            "ready" },
    };
    static char buf[160];
    size_t off = 0;
    int i;

    buf[0] = 0;
    if (!caps) {
        snprintf(buf, sizeof(buf), "none(老模块)");
        return buf;
    }
    for (i = 0; i < (int)(sizeof(tab) / sizeof(tab[0])); i++) {
        if (!(caps & tab[i].bit))
            continue;
        off += (size_t)snprintf(buf + off, sizeof(buf) - off, "%s%s",
                                off ? "," : "", tab[i].name);
        if (off >= sizeof(buf))
            break;
    }
    return buf;
}

static int vdrm_open(void)
{
    if (g_vdrm_fd >= 0) return 0;

    /* /dev/vdrm 的权限由模块 common.sh 的 vdrm_fix_nodes 负责：0666 + gpu_device
     * 标签。故意不做 chown 到本 App uid —— 那会让重装/换 UID 之后打不开，
     * 而 0666 的边界由 SELinux 域来划（有 gpu_device 权限的域才能开）。*/
    g_vdrm_fd = open(VDRM_DEV, O_RDWR | O_CLOEXEC | O_NONBLOCK);
    if (g_vdrm_fd < 0) {
        LOGE("open %s failed err=%d", VDRM_DEV, errno);
        return -1;
    }
    if (g_return_fd < 0)
        g_return_fd = fcntl(g_vdrm_fd, F_DUPFD_CLOEXEC, 0);
    {
        struct vdrm_info info;

        memset(&info, 0, sizeof(info));
        if (ioctl(g_vdrm_fd, VDRM_IOCTL_GET_INFO, &info) == 0)
            LOGI("opened %s fd=%d abi=%u caps=0x%03x [%s]", VDRM_DEV, g_vdrm_fd,
                 info.abi_version, info.flags, vdrm_caps_str(info.flags));
        else
            LOGI("opened %s fd=%d (GET_INFO err=%d)", VDRM_DEV, g_vdrm_fd, errno);
    }
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

/* 通知内核"这一帧已经真的呈现了"。
 * 内核据此在下一个 vblank 完成 flip-done，让合成器的节拍等于真实上屏节拍。*/
struct vdrm_present_req { uint32_t id; uint32_t flags; };
#define VDRM_IOCTL_PRESENTED \
    VDRM_IOC(1, 'V', 0x0e, sizeof(struct vdrm_present_req))
/* 回到前台："把当前这一帧立刻重推给我"。老模块没有它（ENOTTY），忽略即可。*/
#define VDRM_IOCTL_READY \
    VDRM_IOC(1, 'V', 0x0f, sizeof(int))

static void vdrm_presented(int id)
{
    struct vdrm_present_req req;

    memset(&req, 0, sizeof(req));
    req.id = (uint32_t)id;
    if (g_vdrm_fd >= 0)
        ioctl(g_vdrm_fd, VDRM_IOCTL_PRESENTED, &req);
}

/* 让内核把"当前在屏的那一帧"再推一次。
 *
 * 换档时内核在 pipe_enable 里已经推过一次，但那一刻 App 往往还没 forge 完
 * 新尺寸的槽 —— 那帧会被当成 unknown slot 丢掉，而静态桌面换完档不会再有
 * 新提交，于是屏上一直贴着上一档的旧帧（尾帧）。rebuild 完补一发 READY，
 * 这次就能接上。*/
static void vdrm_ready(void)
{
    int32_t one = 1;

    if (g_vdrm_fd >= 0)
        ioctl(g_vdrm_fd, VDRM_IOCTL_READY, &one);
}

static void release_wake(void)
{
    if (g_release_wake_fd >= 0) {
        uint64_t wake = 1;
        (void)write(g_release_wake_fd, &wake, sizeof(wake));
    }
}

/* ---- present fence：SF 说"这一帧真的上屏了"的精确信号 ----
 *
 * 以前 onComplete 一到就报 PRESENTED，然后内核再**猜着**多等一个周期才敢
 * 完成 flip-done。ASurfaceTransactionStats_getPresentFenceFd() 给的 fence
 * 才是真的上屏完成点，所以：
 *
 *   fence 有效（>=0）→ 挂进表里等它 signal 再报（内核随即完成，不多等）；
 *   fence 是 -1（部分设备不支持）→ 立刻报，和以前一模一样。
 *
 * 等待不新起线程：release worker 本来就在 poll 一堆 release fence，顺手
 * 把这个 fd 也塞进同一个 poll 集合。*/
#define PRESENT_MAX 8
#define PRESENT_FENCE_TIMEOUT_MS 2000
static struct {
    bool     used;
    int      fd;
    uint32_t slot;
    uint64_t since_ms;
} g_presents[PRESENT_MAX];

/* 挂进表；表满就当场报 —— presented 绝不能丢，丢了内核只能走 100ms 兜底。*/
static void present_wait_add(uint32_t slot, int fd)
{
    int i, free_i = -1;

    if (fd < 0) {
        vdrm_presented((int)slot);
        return;
    }
    pthread_mutex_lock(&g_display_lock);
    for (i = 0; i < PRESENT_MAX; i++) {
        if (!g_presents[i].used) {
            free_i = i;
            break;
        }
    }
    if (free_i >= 0) {
        g_presents[free_i].used = true;
        g_presents[free_i].fd = fd;
        g_presents[free_i].slot = slot;
        g_presents[free_i].since_ms = now_ms();
    }
    pthread_mutex_unlock(&g_display_lock);

    if (free_i < 0) {
        LOGW("present 表满，slot=%u 直接报", slot);
        close(fd);
        vdrm_presented((int)slot);
        return;
    }
    release_wake();
}

/* worker 侧：这个 fd 报完了，摘表 + 关 fd（fd 的所有权在回调时就给了我们）。*/
static void present_wait_done(int fd)
{
    int i;

    pthread_mutex_lock(&g_display_lock);
    for (i = 0; i < PRESENT_MAX; i++) {
        if (g_presents[i].used && g_presents[i].fd == fd) {
            g_presents[i].used = false;
            break;
        }
    }
    pthread_mutex_unlock(&g_display_lock);
    if (fd >= 0)
        close(fd);
}

/* 停 App / 换 surface 时把还挂着的 present fence 收掉：不报也不再等。*/
static void present_flush(void)
{
    int i;

    pthread_mutex_lock(&g_display_lock);
    for (i = 0; i < PRESENT_MAX; i++) {
        if (!g_presents[i].used)
            continue;
        g_presents[i].used = false;
        if (g_presents[i].fd >= 0)
            close(g_presents[i].fd);
    }
    pthread_mutex_unlock(&g_display_lock);
}

/* 报给内核之前，把 latch -> present 这一段记下来（见上面延迟分解）。*/
static void present_report(uint32_t slot)
{
    uint32_t idx = slot & (LAT_SLOTS - 1);
    uint64_t now = mono_ns();

    if (g_latch_ns[idx] && now > g_latch_ns[idx] &&
        now - g_latch_ns[idx] < 200000000ull) {
        unsigned long long us = (now - g_latch_ns[idx]) / 1000;

        g_l2p_last_us = us;
        if (us > g_l2p_max_us)
            g_l2p_max_us = us;
    }
    g_latch_ns[idx] = 0;
    vdrm_presented((int)slot);
}

static void on_present(void *context, ASurfaceTransactionStats *stats)
{
    uint32_t slot = (uint32_t)(uintptr_t)context;
    uint32_t idx = slot & (LAT_SLOTS - 1);
    static int n;
    int fd = -1;

    if (stats) {
        int64_t latch = ASurfaceTransactionStats_getLatchTime(stats);

        fd = ASurfaceTransactionStats_getPresentFenceFd(stats);
        if (latch > 0 && g_apply_ns[idx]) {
            uint64_t ln = (uint64_t)latch;

            /* 两个时间戳必须是同一个时钟域（CLOCK_MONOTONIC ns）。万一
             * 不是，差值会是天文数字 —— 丢掉并记数，别污染统计。*/
            if (ln > g_apply_ns[idx] && ln - g_apply_ns[idx] < 100000000ull) {
                unsigned long long us = (ln - g_apply_ns[idx]) / 1000;

                g_a2l_last_us = us;
                if (us > g_a2l_max_us)
                    g_a2l_max_us = us;
                g_latch_ns[idx] = ln;
            } else {
                g_lat_rejects++;
            }
            /* 预算第二段：读到帧事件 → apply 落进 SF。*/
            if (g_ev_read_ns && g_apply_ns[idx] > g_ev_read_ns) {
                unsigned long long us = (g_apply_ns[idx] - g_ev_read_ns) / 1000ull;

                g_r2a_last_us = us;
                if (us > g_r2a_max_us)
                    g_r2a_max_us = us;
            }
        }
    }
    if (n++ < 3)
        LOGI("present cb slot=%u fence=%d", slot, fd);
    g_present_ns_arr[slot & 63] = mono_ns();
    present_wait_add(slot, fd);
}

/* 显示完毕，把池 buffer 归还给 KPM（→ FREE），合成器才能再次渲染进去。 */
static void vdrm_return(int id)
{
    struct vdrm_handle_req req;
    memset(&req, 0, sizeof(req));
    req.id = (uint32_t)id;
    if (g_vdrm_fd >= 0)
        ioctl(g_vdrm_fd, VDRM_IOCTL_RETURN, &req);
}/* SF 用完这块 buffer 时回调 → 归还给 KPM（契中台）。可任意线程。 */

#define SUBMISSION_WAIT_MAX 8
/* 等 SF release 的上界。原来是 2000ms —— 那意味着 SF 一旦迟到，这块槽
 * 两秒不回来；池子只有 6 个槽，几帧就能把合成器饿死（实测内核 pace=1/2
 * 配合 2s 超时就是"卡死"）。压到 150ms：最坏情况是我们提前还槽（可能撕
 * 一下），而不是整条管线停住。*/
#define RELEASE_FENCE_TIMEOUT_MS 150

/* 从提交表里摘掉并完成归还。worker 和 worker 起不来时的 fallback 共用。*/
static void submission_finish(struct submission *s)
{
    submission_release(s, true);
}

/* 放弃跟踪，但**不**把槽还给池。
 *
 * 用在"等不到 SF 的 release fence / 回调"的时候：那时这块 buffer 很可能还在
 * 扫描输出，还给池就等于把屏上的内存交给别人画 —— 屏上闪一下。
 *
 * 槽不会漏：内核在下一帧真正上屏时会把它顶下来的 DISPL 槽自动置 FREE
 * （vdrm_pipe_update 里的回收），那个信号才是权威的。*/
static void submission_abandon(struct submission *s)
{
    submission_release(s, false);
}

static void submission_release(struct submission *s, bool give_back)
{
    struct submission **p;
    bool still_used = false;

    if (!s)
        return;

    pthread_mutex_lock(&g_display_lock);
    p = &g_submissions;
    while (*p && *p != s)
        p = &(*p)->next;
    if (!*p) {
        pthread_mutex_unlock(&g_display_lock);
        return;   /* 已被别的路径完成 */
    }
    *p = s->next;
    for (struct submission *other = g_submissions; other; other = other->next)
        if (other->slot == s->slot) { still_used = true; break; }
    if (give_back && !still_used) {
        struct vdrm_handle_req req = { .id = s->slot };
        uint64_t pn = g_present_ns_arr[s->slot & 63];

        if (pn) {
            uint64_t rn = mono_ns();

            if (rn > pn) {
                unsigned long long us = (rn - pn) / 1000ull;

                g_p2t_last_us = us;
                if (us > g_p2t_max_us)
                    g_p2t_max_us = us;
            }
            g_present_ns_arr[s->slot & 63] = 0;
        }
        int rfd = (g_return_fd >= 0) ? g_return_fd : g_vdrm_fd;
        if (ioctl(rfd, VDRM_IOCTL_RETURN, &req) < 0)
            LOGW("RETURN slot=%u err=%d", s->slot, errno);
    }
    pthread_mutex_unlock(&g_display_lock);

    if (s->fence_fd >= 0)
        close(s->fence_fd);
    AHardwareBuffer_release(s->ahb);
    atomic_fetch_sub(&g_submissions_live, 1);
    if (g_release_wake_fd >= 0) {
        uint64_t wake = 1;
        (void)write(g_release_wake_fd, &wake, sizeof(wake));
    }

    /* 对象回缓存，下一帧直接复用；token 清零后迟到的回调会被忽略。 */
    s->ahb = NULL;
    s->fence_fd = -2;
    s->slot = 0;
    s->token = 0;
    pthread_mutex_lock(&g_display_lock);
    s->next = g_submission_free;
    g_submission_free = s;
    pthread_mutex_unlock(&g_display_lock);
}

static struct submission *submission_create(AHardwareBuffer *ahb, uint32_t slot)
{
    struct submission *s;
    int i;

    pthread_mutex_lock(&g_display_lock);
    if (!g_submission_cache_ready) {
        for (i = 0; i < SUBMISSION_CACHE_MAX; i++) {
            g_submission_cache[i].next = g_submission_free;
            g_submission_free = &g_submission_cache[i];
        }
        g_submission_cache_ready = true;
    }
    s = g_submission_free;
    if (s)
        g_submission_free = s->next;
    pthread_mutex_unlock(&g_display_lock);

    if (!s) {
        s = calloc(1, sizeof(*s));
        if (!s) return NULL;
    }

    s->ahb = ahb; s->slot = slot;
    s->fence_fd = -2;   /* release callback 还没到 */
    s->token = atomic_fetch_add(&g_sub_token, 1) + 1;
    s->created_ms = now_ms();
    AHardwareBuffer_acquire(ahb);

    pthread_mutex_lock(&g_display_lock);
    s->next = g_submissions; g_submissions = s;
    atomic_fetch_add(&g_submissions_live, 1);
    pthread_mutex_unlock(&g_display_lock);
    return s;
}

static size_t submission_count(void)
{
    return atomic_load(&g_submissions_live);
}

static void drain_eventfd(int fd)
{
    uint64_t v;
    while (read(fd, &v, sizeof(v)) > 0) { }
}

static void *release_worker(void *arg)
{
    (void)arg;

    for (;;) {
        struct submission *wait[SUBMISSION_WAIT_MAX];
        struct submission *imm[SUBMISSION_WAIT_MAX];
        struct submission *stuck[SUBMISSION_WAIT_MAX];
        struct pollfd pfds[SUBMISSION_WAIT_MAX + PRESENT_MAX + 1];
        uint32_t pslot[PRESENT_MAX];
        uint64_t psince[PRESENT_MAX];
        int wait_n = 0, imm_n = 0, stuck_n = 0, pres_n = 0, i, r;
        uint64_t now = now_ms();

        pthread_mutex_lock(&g_display_lock);
        for (struct submission *s = g_submissions; s; s = s->next) {
            if (s->fence_fd == -1) {
                if (imm_n < SUBMISSION_WAIT_MAX)
                    imm[imm_n++] = s;
            } else if (s->fence_fd >= 0 &&
                       wait_n < SUBMISSION_WAIT_MAX) {
                wait[wait_n] = s;
                pfds[wait_n].fd = s->fence_fd;
                pfds[wait_n].events = POLLIN;
                pfds[wait_n].revents = 0;
                wait_n++;
            } else if (s->fence_fd == -2 &&
                       now - s->created_ms > SUB_STUCK_MS &&
                       stuck_n < SUBMISSION_WAIT_MAX) {
                /* SF 一直没回调（图层被 reparent 掉等）：不能等它，
                 * 否则 g_submissions_live 只增不减，渲染节流会把帧全丢掉。
                 *
                 * 但**当前在屏的那一块不能动**：画面静止时 SF 会一直攥着它
                 * （要等下一帧替换才回调），这时候强行 RETURN 等于告诉驱动
                 * "这块空出来了"，驱动随后可能把同一片内存再发给合成器 ——
                 * 屏上还显示着它，于是闪一下。留着不退；等下一帧替换时 SF
                 * 自然会回调，那时才真正归还。*/
                stuck[stuck_n++] = s;
            }
        }
        /* 顺手把在等的 present fence 一起收进来：同一个 poll 集合里等，
         * 省一个线程。索引 = wait_n + pres_n，两边互不干扰。*/
        for (i = 0; i < PRESENT_MAX && pres_n < PRESENT_MAX; i++) {
            if (!g_presents[i].used || g_presents[i].fd < 0)
                continue;
            pslot[pres_n] = g_presents[i].slot;
            psince[pres_n] = g_presents[i].since_ms;
            pfds[wait_n + pres_n].fd = g_presents[i].fd;
            pfds[wait_n + pres_n].events = POLLIN;
            pfds[wait_n + pres_n].revents = 0;
            pres_n++;
        }
        pthread_mutex_unlock(&g_display_lock);

        for (i = 0; i < imm_n; i++)
            submission_finish(imm[i]);
        for (i = 0; i < stuck_n; i++) {
            /* SF 一直不回调：这块很可能还在扫描输出。**不还池**，只放弃
             * 跟踪 —— 槽由内核在下一帧上屏时回收。*/
            LOGW("release callback missing slot=%u; hold until late callback",
                 stuck[i]->slot);
            held_add(stuck[i]->token, stuck[i]->slot);
            submission_abandon(stuck[i]);
        }

        held_expire();   /* 等不到回调的槽：超时才强制归还 */

        if (wait_n == 0 && pres_n == 0) {
            struct pollfd w = { .fd = g_release_worker_fd, .events = POLLIN };
            /* 没有在飞的 submission 时 1s 才醒一次；有 submission
             * （可能还没进 wait/pres 的 stuck 项）才用 100ms 扫描。*/
            int tmo = submission_count() ? 100 : 1000;
            r = poll(&w, 1, tmo);
            if (r > 0)
                drain_eventfd(g_release_worker_fd);
            continue;
        }

        {
            int total = wait_n + pres_n;

            pfds[total].fd = g_release_worker_fd;
            pfds[total].events = POLLIN;
            pfds[total].revents = 0;
            r = poll(pfds, total + 1, 250);
            if (r < 0) {
                if (errno == EINTR)
                    continue;
                LOGW("release worker poll err=%d; forcing all", errno);
                for (i = 0; i < wait_n; i++)
                    submission_finish(wait[i]);
                continue;
            }
            if (r > 0 && (pfds[total].revents & POLLIN))
                drain_eventfd(g_release_worker_fd);
        }

        now = now_ms();
        for (i = 0; i < wait_n; i++) {
            if (r > 0 && (pfds[i].revents &
                          (POLLIN | POLLERR | POLLHUP | POLLNVAL))) {
                submission_finish(wait[i]);
            } else if (now - wait[i]->created_ms > SUB_FENCE_TIMEOUT_MS) {
                /* fence 迟迟不 signal 在静态画面下是**常态**：HWC 直接拿这块
                 * 做 overlay 扫描输出，SF 的 fence 要等下一帧把它换下来才
                 * signal。所以既不能等它（会卡住提交表），也不能还池
                 * （那块可能还在屏上）—— 只放弃跟踪，槽交给内核在下一帧
                 * 上屏时回收。*/
                LOGW("release fence timeout slot=%u; hold until late callback",
                     wait[i]->slot);
                held_add(wait[i]->token, wait[i]->slot);
                submission_abandon(wait[i]);
            }
        }

        /* present fence：signal 了就是"真的上屏了"，这时才报内核。
         * 它比 onComplete 晚（前者是扫描输出完成，后者是"提交进这一拍合成"），
         * 正好是内核完成 flip-done 需要的那个时刻 —— 所以内核不用再猜一个周期。*/
        for (i = 0; i < pres_n; i++) {
            int idx = wait_n + i;
            bool hit = (r > 0 && (pfds[idx].revents &
                                  (POLLIN | POLLERR | POLLHUP | POLLNVAL)));

            if (hit) {
                present_report(pslot[i]);
                present_wait_done(pfds[idx].fd);
            } else if (now - psince[i] > PRESENT_FENCE_TIMEOUT_MS) {
                /* 一直不 signal（设备不支持 / HWC 吐了个不 signal 的 fd）：
                 * 还是要报，否则内核只能等 100ms 兜底，整条链掉到 10fps。*/
                LOGW("present fence timeout slot=%u; 先报", pslot[i]);
                present_report(pslot[i]);
                present_wait_done(pfds[idx].fd);
            }
        }
    }
    return NULL;
}

static void release_worker_init(void)
{
    pthread_t t;

    g_release_worker_fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (g_release_worker_fd < 0) {
        LOGE("release worker eventfd failed err=%d", errno);
        return;
    }
    if (pthread_create(&t, NULL, release_worker, NULL) != 0) {
        LOGE("release worker thread failed err=%d", errno);
        close(g_release_worker_fd);
        g_release_worker_fd = -1;
        return;
    }
    pthread_detach(t);
    g_release_worker_ok = true;
    LOGI("release worker started");
}

/* worker 起不来时的 fallback：带超时，避免永久挂住。*/
static void *release_wait(void *arg)
{
    struct submission *s = arg;

    if (s->fence_fd >= 0) {
        struct pollfd pfd = { .fd = s->fence_fd, .events = POLLIN };
        int r;
        do {
            r = poll(&pfd, 1, RELEASE_FENCE_TIMEOUT_MS);
        } while (r < 0 && errno == EINTR);
        if (r == 0)
            LOGW("release fence timeout slot=%u; forcing RETURN", s->slot);
        else if (r < 0 || !(pfd.revents & POLLIN))
            LOGW("release fence failed slot=%u; forcing RETURN", s->slot);
    }
    submission_finish(s);
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

/* ---- 等不到 SF release 回调时，槽的所有权归谁 ----
 *
 * 旧做法：「放弃跟踪、不还池，让内核在下一帧上屏时回收」。这在 HWC overlay
 * 链路上是错的：SF 把这块 buffer 交给 DPU 直接扫描之后，还要多攥 1–2 帧才
 * release；内核却在下一帧上屏时就把槽判成 FREE，合成器立刻画进去 —— 屏幕上
 * 正在扫的那块显存被改写，表现为"局部蓝块"，而且录屏抓不到（录屏走另一条
 * 合成路径，读的是改写之后的像素）。
 *
 * 现在：按 token 记住 (token, slot)，等迟到的回调到了再 RETURN；只有超过
 * HELD_TIMEOUT_MS 才强制归还（日志会喊）。静态画面下 SF 一直攥着最后一块是
 * 正常的 —— 不还就行，反正没有新帧要画。*/
#define HELD_MAX 64
#define HELD_TIMEOUT_MS 5000
static struct {
    uint64_t token;
    uint32_t slot;
    uint64_t since_ms;
} g_held[HELD_MAX];

static void held_add(uint64_t token, uint32_t slot)
{
    uint64_t now = now_ms();
    int i, free_i = -1;

    if (!token)
        return;
    pthread_mutex_lock(&g_display_lock);
    for (i = 0; i < HELD_MAX; i++) {
        if (g_held[i].token == token)
            break;
        if (g_held[i].token == 0 && free_i < 0)
            free_i = i;
    }
    if (i >= HELD_MAX && free_i >= 0) {
        g_held[free_i].token = token;
        g_held[free_i].slot = slot;
        g_held[free_i].since_ms = now;
    } else if (i >= HELD_MAX) {
        LOGW("held table full; slot %u waits for kernel reclaim", slot);
    }
    pthread_mutex_unlock(&g_display_lock);
}

/* 迟到的回调到了：归还槽。返回 true 表示这个 token 本来在"等回调"表里。*/
static bool held_take_return(uint64_t token)
{
    uint32_t slot = 0;
    bool hit = false;
    int i;

    pthread_mutex_lock(&g_display_lock);
    for (i = 0; i < HELD_MAX; i++) {
        if (g_held[i].token == token) {
            slot = g_held[i].slot;
            g_held[i].token = 0;
            hit = true;
            break;
        }
    }
    pthread_mutex_unlock(&g_display_lock);
    if (hit) {
        LOGI("late release -> return slot=%u (token=%llu)", slot,
             (unsigned long long)token);
        vdrm_return((int)slot);
    }
    return hit;
}

/* 兜底：真等不到回调/ fence 时强制归还（并吼一声）*/
/* force=1：不等超时，把表里所有"等 release"的槽立刻还给内核。
 * 从后台回到前台、或者 SurfaceControl 重建时要这么干 —— 否则 SF 在后台
 * 不会给 release 回调，池槽一直挂在 DISPL，内核 dumb_create 会傻等 6 秒，
 * 表现就是"切回前台冻住七八秒"（实测 evgap_max=7241ms）。*/
static void held_expire_force(bool force)
{
    uint32_t slots[HELD_MAX];
    int n = 0, i;
    uint64_t now = now_ms();

    pthread_mutex_lock(&g_display_lock);
    for (i = 0; i < HELD_MAX; i++) {
        if (g_held[i].token && (force ||
            now - g_held[i].since_ms > HELD_TIMEOUT_MS)) {
            slots[n++] = g_held[i].slot;
            g_held[i].token = 0;
        }
    }
    pthread_mutex_unlock(&g_display_lock);
    for (i = 0; i < n; i++) {
        LOGW("held slot %u %s -> return", slots[i],
             force ? "(resume: forced)" : "(expired)");
        vdrm_return((int)slots[i]);
    }
}

static void held_expire(void)
{
    held_expire_force(false);
}

static void on_buf_release(void *context, int release_fd)
{
    uint64_t token = (uint64_t)(uintptr_t)context;
    struct submission *s = NULL;

    /* sc_remove() 用 NULL buffer 清图层时也会走到这里（token=0） */
    if (!token)
        return;

    pthread_mutex_lock(&g_display_lock);
    for (struct submission *p = g_submissions; p; p = p->next) {
        if (p->token == token) { s = p; break; }
    }
    if (s)
        s->fence_fd = release_fd;
    pthread_mutex_unlock(&g_display_lock);

    if (!s) {
        /* 迟到的回调：先在"等回调"表里找。找到就说明 SF 终于放手了，
         * 现在才能把这个槽还给池（在此之前还 = 把屏上的内存交给别人画）。*/
        if (held_take_return(token)) {
            if (release_fd >= 0)
                close(release_fd);
            return;
        }
        /* 真·过期回调（那一帧早被强制回收、块也复用给别的帧了）：
         * 只把这个 fd 关掉，绝不碰任何 submission。*/
        LOGW("late release callback (token=%llu); fd=%d closed",
             (unsigned long long)token, release_fd);
        if (release_fd >= 0)
            close(release_fd);
        return;
    }

    if (g_release_worker_ok) {
        uint64_t wake = 1;
        if (g_release_worker_fd >= 0)
            (void)write(g_release_worker_fd, &wake, sizeof(wake));
        return;
    }

    if (release_fd < 0) {
        submission_finish(s);
        return;
    }
    pthread_t thread;
    if (pthread_create(&thread, NULL, release_wait, s) == 0)
        pthread_detach(thread);
    else
        release_wait(s);
}/* 注入一个输入事件给 KPM 的虚拟输入设备 *//* ======================== buffer collection ======================== */

/* 归还并释放当前这一组 AHB。池槽本身由内核/合成器会话管理，这里只放
 * App 自己持有的 AHB 引用。*/
static void free_buffers(struct consumer *c)
{
    for (int i = 0; i < c->buf_count; i++) {
        if (c->bufs[i].ahb)
            AHardwareBuffer_release(c->bufs[i].ahb);
        c->bufs[i].ahb = NULL;
        c->bufs[i].slot = -1;
    }
    if (c->buf_count)
        LOGI("freed %d buffers", c->buf_count);
    c->buf_count = 0;
    g_last_ahb = NULL;
    g_last_slot = g_last_w = g_last_h = 0;
}

/* 关掉上一轮的 eventfd，避免反复 surfaceChanged 时泄漏 fd。 */
static void close_channels(struct consumer *c)
{
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
                           AHARDWAREBUFFER_USAGE_GPU_COLOR_OUTPUT |
                           AHARDWAREBUFFER_USAGE_COMPOSER_OVERLAY;

    n = vdrm_get_pool(&lst);
    if (n <= 0) {
        LOGE("forge_pool: KPM pool empty (n=%d)", n);
        return -1;
    }
    c->buf_count = 0;
    /* 【遍历整个池，不能用 i < VDR2_BUFS 截断】。
     *
     * 池是"按档累积"的：每试一个新分辨率就多 6 块，旧档的槽留着以便切回去。
     * 试过 3 档就有 18 块，当前档那 6 块的**下标在 16 之后** —— 以前这里用
     * `i < VDR2_BUFS`（16）截断，于是只看了前 16 条（全是别的尺寸，被尺寸
     * 过滤掉），一块都没 forge，render loop 随即满屏 "unknown frame slot"，
     * 画面停在上一帧或直接黑屏。现在按"能装下几块"限制，而不是"看前几条"。*/
    for (i = 0; i < n; i++) {
        if (lst.items[i].fd < 0)
            continue;
        /* 装满了：剩下的条目只是把 fd 关掉，不再 forge */
        if (c->buf_count >= VDR2_BUFS) {
            close(lst.items[i].fd);
            if (lst.items[i].fd1 >= 0) close(lst.items[i].fd1);
            continue;
        }
        /* mode 切换过渡期：池里可能还留着旧尺寸的批次。只 forge 当前
         * c->mode_* 的条目，否则 render loop 会把旧 id 当新帧、
         * 卡在 "unknown frame slot" 上。*/
        if (c->mode_w > 0 && c->mode_h > 0 &&
            ((int)lst.items[i].width != c->mode_w ||
             (int)lst.items[i].height != c->mode_h)) {
            close(lst.items[i].fd);
            if (lst.items[i].fd1 >= 0) close(lst.items[i].fd1);
            continue;
        }
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
        c->bufs[c->buf_count].ahb  = ahb;
        c->bufs[c->buf_count].slot = (int)lst.items[i].id;
        LOGD("  forged[%d]: id=%u %ux%u pitch=%u ahb=%p", i,
             lst.items[i].id, lst.items[i].width, lst.items[i].height,
             lst.items[i].pitch, (void *)ahb);
        c->buf_count++;
    }
    LOGI("forge_pool: %d forged from KPM pool (of %d)", c->buf_count, n);
    return c->buf_count > 0 ? 0 : -1;
}

/* ======================== render loop ======================== */

/* ==================== vsync tick（对齐 anland） ====================
 *
 * anland 的做法是「一 vsync 一 latch」：节拍来自 Android 的 AChoreographer，
 * 客户端每拍最多提交一帧。我们原来只靠 in-flight 计数兜着，合成器推得比 SF
 * 消费快时就会反复覆写还在扫描输出的槽（frames_reuse 高、面板闪）。
 *
 * 这里起一个独立线程：prepare 自己的 ALooper → AChoreographer 每拍回调 →
 * 记 seq、写 eventfd 叫醒渲染循环。渲染循环只在 seq 前进时才允许提交；
 * 100ms 没有新拍（或 tick 通道不可用）就放行，避免把合成器饿死。
 */
static int              g_tick_fd = -1;
static volatile int     g_tick_on;
static volatile int     g_tick_seq;
static volatile uint64_t g_tick_ms;
static int              g_tick_used = -1;   /* -1：这一拍还没贴过 */
static AChoreographer  *g_choreo;
static ALooper         *g_tick_looper;
static pthread_t        g_tick_tid;
static int              g_tick_started;

static void tick_cb(int64_t frame_time_ns, void *data)
{
    (void)frame_time_ns;
    (void)data;
    __atomic_add_fetch(&g_tick_seq, 1, __ATOMIC_RELAXED);
    g_tick_ms = now_ms();
    g_panel_vsync_ns = mono_ns();
    if (g_tick_fd >= 0) {
        uint64_t one = 1;
        ssize_t r = write(g_tick_fd, &one, sizeof(one));
        (void)r;
    }
    if (g_tick_on && g_choreo)
        AChoreographer_postFrameCallback(g_choreo, tick_cb, NULL);
}

static void *tick_thread(void *arg)
{
    (void)arg;
    g_tick_looper = ALooper_prepare(ALOOPER_PREPARE_ALLOW_NON_CALLBACKS);
    if (!g_tick_looper) {
        LOGE("tick: ALooper_prepare failed");
        return NULL;
    }
    g_choreo = AChoreographer_getInstance();
    if (!g_choreo) {
        LOGE("tick: AChoreographer unavailable");
        return NULL;
    }
    AChoreographer_postFrameCallback(g_choreo, tick_cb, NULL);
    while (g_tick_on)
        ALooper_pollOnce(-1, NULL, NULL, NULL);
    return NULL;
}

/* 每 ~500ms 跑一次：读内核 tick 时刻，和面板 vsync 比，调相位。
 * 目标相位 TARGET_NS：让 tick 比面板 vsync 提前这么多。*/
#define VDRM_PLL_TARGET_NS   (-2000000LL)   /* tick 早 2ms */
#define VDRM_PLL_GAIN        0.3

static void pll_update(void)
{
    struct vdrm_tick_info ti;
    int64_t period, delta, err, want;

    if (!g_pll_enabled)
        return;
    if (g_vdrm_fd < 0)
        return;
    if (ioctl(g_vdrm_fd, VDRM_IOCTL_TICK_INFO, &ti) < 0 || !ti.running)
        return;
    if (!g_panel_vsync_ns || !ti.last_tick_ns || !ti.period_ns)
        return;

    period = (int64_t)ti.period_ns;
    /* tick 相对面板 vsync 的相位，折到 [-period/2, period/2) */
    delta = (int64_t)(ti.last_tick_ns - (int64_t)g_panel_vsync_ns);
    while (delta >= period / 2)
        delta -= period;
    while (delta < -period / 2)
        delta += period;
    g_pll_delta_ns = delta;

    err = VDRM_PLL_TARGET_NS - delta;
    want = ti.phase_ns + (int64_t)(err * VDRM_PLL_GAIN);
    if (want > period)
        want = period;
    if (want < -period)
        want = -period;
    if (want != ti.phase_ns) {
        if (ioctl(g_vdrm_fd, VDRM_IOCTL_TICK_PHASE, &want) == 0)
            g_pll_phase_ns = want;
    }
}

static void tick_start(void)
{
    if (g_tick_started)
        return;
    g_tick_fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (g_tick_fd < 0) {
        LOGE("tick: eventfd failed err=%d", errno);
        return;
    }
    g_tick_on = 1;
    if (pthread_create(&g_tick_tid, NULL, tick_thread, NULL) != 0) {
        LOGE("tick: pthread_create failed");
        g_tick_on = 0;
        close(g_tick_fd);
        g_tick_fd = -1;
        return;
    }
    g_tick_started = 1;
    LOGI("tick: vsync gating on (one latch per tick)");
}

static void tick_stop(void)
{
    if (!g_tick_started)
        return;
    g_tick_on = 0;
    if (g_tick_looper)
        ALooper_wake(g_tick_looper);
    if (g_tick_fd >= 0) {
        uint64_t one = 1;
        ssize_t r = write(g_tick_fd, &one, sizeof(one));
        (void)r;
    }
    pthread_join(g_tick_tid, NULL);
    if (g_tick_fd >= 0) {
        close(g_tick_fd);
        g_tick_fd = -1;
    }
    g_tick_started = 0;
}

static void *render_loop(void *arg)
{
    struct consumer *c = arg;
    LOGI("render loop started");
    tick_start();
    int loop_count = 0;
    uint64_t last_event_ms = now_ms();
    uint64_t stat_ev = 0, stat_show = 0;
    uint32_t prev_show_seq = 0;
    struct vdrm_frame_event pending = {0};
    bool have_pending = false;
    /* 空闲/恢复后的第一帧不等下一拍，直接 latch；后续仍一 tick 一 latch。*/
    bool force_latch = false;
    uint64_t stat_deferred = 0;
    time_t stat_t0 = time(NULL);
    uint64_t last_mode_check_ms = 0;

    while (c->running) {
        loop_count++;

        if (loop_count <= 3)
            LOGI("hb loop=%d running=%d", loop_count, c->running ? 1 : 0);

        if (time(NULL) - stat_t0 >= 2) {
            LOGI("STATS events=%llu shown=%llu deferred=%llu outstanding=%zu over %lds"
                 " | apply2latch=%lluus(max %llu) latch2present=%lluus(max %llu)"
                 " | push2read=%lluus(max %llu) read2apply=%lluus(max %llu)"
                 " present2ret=%lluus(max %llu)"
                 " | pll_phase=%lldus delta=%lldus"
                 " | evgap_max=%llums n40=%llu n80=%llu latch_max=%lluus%s",
                 (unsigned long long)stat_ev, (unsigned long long)stat_show,
                 (unsigned long long)stat_deferred, submission_count(),
                 (long)(time(NULL) - stat_t0),
                 g_a2l_last_us, g_a2l_max_us, g_l2p_last_us, g_l2p_max_us,
                 g_p2r_last_us, g_p2r_max_us, g_r2a_last_us, g_r2a_max_us,
                 g_p2t_last_us, g_p2t_max_us,
                 (long long)(g_pll_phase_ns / 1000), (long long)(g_pll_delta_ns / 1000),
                 g_ev_gap_max_ms, g_ev_gap_n40, g_ev_gap_n80, g_latch_max_us,
                 g_lat_rejects ? " [lat clock mismatch!]" : "");
            stat_ev = 0; stat_show = 0; stat_deferred = 0;
            g_ev_gap_max_ms = 0; g_ev_gap_n40 = 0; g_ev_gap_n80 = 0;
            g_latch_max_us = 0;
            stat_t0 = time(NULL);
        }

        /* 1 秒兜底。正常换档走下面的 eventfd，被内核立刻叫醒；
         * 这里只防事件丢失/老模块没有事件通道。*/
        {
            uint64_t now = now_ms();
            if (now - last_mode_check_ms >= 1000) {
                last_mode_check_ms = now;
                maybe_rebuild_pool(c);
            }
            if (now - g_pll_last_ms >= 500) {
                g_pll_last_ms = now;
                pll_update();
            }
        }

        /* 等 KPM 的帧事件：直接 poll/read /dev/vdrm 自带的队列。
         * （不再依赖 KPM 写 App 的 memfd——那条路内核侧 VFS 会 EACCES。）*/
    struct pollfd pfds[5] = {
        { .fd = g_vdrm_fd, .events = POLLIN },
        { .fd = c->stop_fd, .events = POLLIN },
        { .fd = g_release_wake_fd, .events = POLLIN },
        { .fd = c->event_fd, .events = POLLIN },
        { .fd = g_tick_fd, .events = POLLIN }
    };
    /* 500ms 超时只是兜底；正常由帧事件 / mode eventfd / vsync tick 唤醒 */
    int pr = poll(pfds, 5, 500);
        if (!c->running || pfds[1].revents) break;
        if (pfds[2].revents) {
            uint64_t wake;
            while (read(g_release_wake_fd, &wake, sizeof(wake)) > 0) {}
        }
        if (pfds[4].revents) {
            uint64_t tk;
            while (read(g_tick_fd, &tk, sizeof(tk)) > 0) {}
        }
        /* 内核可能刚请求了"这一档还没有槽"：补池（只在 gen 变化时拉一次） */
        maybe_request_pool();
        if (pfds[3].revents) {
            uint64_t ev;

            while (read(c->event_fd, &ev, sizeof(ev)) > 0) {}
            /* commit：Java 提前转窗口；pipe_enable：Native 重建池/几何。
             * 两个都做一遍，幂等。*/
            maybe_rebuild_pool(c);
            notify_mode_changed();
        }
        if (pr <= 0) {
            maybe_rebuild_pool(c);
            continue;
        }

        /* 排空事件队列，只认最新的一帧（latest wins）。
         * 逐条 FIFO 消费时，一旦有小突发就会去显示已过期的旧帧 → 视觉上"倒带重播"。*/
        struct vdrm_frame_event ev, latest;
        int got = 0;
        uint64_t ev_gap_ms = 0;
        bool first_ev = false;
        {
            uint64_t n = mono_ns();

            first_ev = (g_last_ev_ns == 0);
            if (!first_ev) {
                unsigned long long ms = (n - g_last_ev_ns) / 1000000ull;

                ev_gap_ms = ms;
                if (ms > g_ev_gap_max_ms)
                    g_ev_gap_max_ms = ms;
                if (ms > 40)
                    g_ev_gap_n40++;
                if (ms > 80)
                    g_ev_gap_n80++;
            }
            g_last_ev_ns = n;
        }
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
            /* 预算第一段：内核 pipe_update 推帧 → 我们读到。事件的
             * timestamp_ns 由 vdrm_frame_push() 用 ktime_get() 打。*/
            {
                uint64_t rn = mono_ns();

                g_ev_read_ns = rn;
                if (ev.timestamp_ns && rn > ev.timestamp_ns) {
                    unsigned long long us = (rn - ev.timestamp_ns) / 1000ull;

                    g_p2r_last_us = us;
                    if (us > g_p2r_max_us)
                        g_p2r_max_us = us;
                }
            }
        }
        if (got) {
            if (have_pending && pending.id != latest.id)
                return_unsubmitted(pending.id);
            pending = latest;
            have_pending = true;
            for (int i = 0; i < drained_count; i++)
                if (drained_ids[i] != pending.id) return_unsubmitted(drained_ids[i]);
            stat_ev++;
            last_event_ms = now_ms();
            if (!g_tick_started)
                tick_start();
            /* 首帧，或距离上一帧 >=50ms（空闲后第一次输入/恢复）：不等 tick。*/
            force_latch = first_ev || ev_gap_ms >= 50;
        }
        if (!have_pending) {
            /* 空闲 500ms 后停掉 Choreographer 回调线程；有帧事件再拉起。*/
            if (g_tick_started && now_ms() - last_event_ms > 500)
                tick_stop();
            continue;
        }

        /* anland 对齐：一 vsync 一 latch。
         * 没有新的 Choreographer 拍就把 pending 留着（poll 会被 tick_fd 叫醒），
         * 超过 100ms 没拍就放行兜底；tick 通道起不来时完全不拦。*/
        if (force_latch) {
            /* 空闲后的首帧直接 latch，救掉最多一整个 vsync 的延迟。
             * 把 tick 游标对齐到当前拍，下一帧仍然是一 tick 一 latch，
             * 不会连续两帧贴进同一拍。*/
            g_tick_used = __atomic_load_n(&g_tick_seq, __ATOMIC_RELAXED);
            force_latch = false;
        } else if (g_tick_started && g_tick_on) {
            int tk = __atomic_load_n(&g_tick_seq, __ATOMIC_RELAXED);

            /* 旧写法是"没等到新的一拍就先憋着"，等于每帧都延后到下一拍
             * 才 apply —— 而 AChoreographer 的回调本来就在 vsync 边界上，
             * SF 的 deadline 在它之前，于是每帧都错过本帧、白等一个 vsync
             * （实测 apply2latch 7–11ms、latch2present 13ms）。
             *
             * 反过来：这一拍还没贴过就立刻贴，贴过才等下一拍。节拍仍然是
             * 一 vsync 一帧，但落点从"下一拍的边界"提前到"帧事件到达时"。*/
            if (tk == g_tick_used)
                continue;
            g_tick_used = tk;
        }

        /* in-flight 上限。之前写死 3：宿主 120Hz、合成器推 118 帧时，App 会
         * 被这个上限卡在 ~80 帧，贴帧间隔不均匀（慢速动画上就是"一顿一顿"）。
         * 池里有 6 个槽，放宽到 4 更平滑；可用 debug.vdrm.inflight 调着试。*/
        if (submission_count() >= max_inflight()) {
            stat_deferred++;
            continue;
        }
        ev = pending;

        uint32_t seq = ev.sequence;
        if (loop_count % 50 == 1)
            LOGI("iter loop=%d seq=%u slot=%u last=%u",
                 loop_count, seq, ev.id, c->last_seq);
        if (seq == c->last_seq) {
            /* 内核已经把槽标成 DISPL；不归还就会永久占住一个槽。*/
            have_pending = false;
            return_unsubmitted(ev.id);
            continue;
        }
        uint32_t target_slot = ev.id;

        /* slot 是 KPM 的全局池 id（跨 App 重启递增），不是数组下标，
         * 必须查表确认它属于我们注册的某块 buffer。*/
        int known = 0;
        for (int i = 0; i < c->buf_count; i++)
            if (c->bufs[i].slot == (int)target_slot) { known = 1; break; }
        if (!known) {
            /* 池刚换过一批（换档 / mkpool 补槽）：先对一次账。
             * 旧写法只在 buf_count==0 时 forge；旧列表还在时新 id 永远对
             * 不上，换档后新尺寸的帧会被一直当成 unknown 丢掉。*/
            maybe_rebuild_pool(c);
            for (int i = 0; i < c->buf_count; i++)
                if (c->bufs[i].slot == (int)target_slot) { known = 1; break; }
        }
        if (!known) {
            /* 同尺寸的新批次（mkpool --reset / 重新补槽）：旧 AHB 列表里
             * 没有这个 id，丢掉重 forge 一次。限频，避免每个过期 id 都刷。*/
            static uint64_t last_reforge_ms;
            uint64_t now = now_ms();

            if (now - last_reforge_ms >= 500) {
                last_reforge_ms = now;
                pthread_mutex_lock(&g_display_lock);
                free_buffers(c);
                if (forge_pool(c) == 0)
                    LOGI("re-forge ok: %d bufs (slot %u)",
                         c->buf_count, target_slot);
                pthread_mutex_unlock(&g_display_lock);
                for (int i = 0; i < c->buf_count; i++)
                    if (c->bufs[i].slot == (int)target_slot) { known = 1; break; }
            }
        }
        if (!known) {
            LOGW("unknown frame slot=%u loop=%d", target_slot, loop_count);
            return_unsubmitted(target_slot);
            continue;
        }

        AHardwareBuffer *ahb = NULL;
        for (int i = 0; i < c->buf_count; i++)
            if (c->bufs[i].slot == (int)target_slot) { ahb = c->bufs[i].ahb; break; }
        if (!ahb) {
            LOGW("no ahb for slot=%u", target_slot);
            return_unsubmitted(target_slot);
            continue;
        }

        if (atomic_load(&g_paused)) {
            /* 后台：不 latch、不拷贝，槽立刻还内核，回前台统一 READY。*/
            have_pending = false;
            return_unsubmitted(target_slot);
            continue;
        }

        LDBG("setbuf slot=%u ahb=%p", target_slot, (void *)ahb);
        pthread_mutex_lock(&g_geometry_lock);
        /* 尾帧护栏：内核在 pipe_update 里已经按 g_active 过滤过一道，这里
         * 再兜一层（池批次/时序竞争时仍可能拿到旧尺寸）。尺寸和当前 mode
         * 不一致的帧不贴、也绝不反过来修改 c->mode —— 那正是“尾帧把刚切
         * 过去的新档回滚”的那条路。*/
        {
            AHardwareBuffer_Desc d;

            memset(&d, 0, sizeof(d));
            AHardwareBuffer_describe(ahb, &d);
            if (d.width && d.height &&
                ((int)d.width != c->mode_w || (int)d.height != c->mode_h)) {
                LOGW("drop stale frame %ux%u (mode %dx%d)",
                     d.width, d.height, c->mode_w, c->mode_h);
                pthread_mutex_unlock(&g_geometry_lock);
                return_unsubmitted(target_slot);
                continue;
            }
        }

        bool shown = sc_latch(ahb, target_slot, (uint32_t)c->mode_w,
                              (uint32_t)c->mode_h,
                              (uint32_t)c->screen_w, (uint32_t)c->screen_h);
        pthread_mutex_unlock(&g_geometry_lock);
        if (!shown) {
            /* 上屏失败（SC 还没建好等）：把槽还回去，别泄漏。*/
            return_unsubmitted(target_slot);
            continue;
        }
        c->last_seq = seq;
        have_pending = false;
        if (seq < prev_show_seq)
            LOGW("SEQ BACKWARD prev=%u now=%u", prev_show_seq, seq);
        prev_show_seq = seq;
        LDBG("setbuf applied slot=%u seq=%u", target_slot, seq);
        if (loop_count % 60 == 1)
            LOGI("show slot=%u %ux%u seq=%u loop=%d",
                 target_slot, c->mode_w, c->mode_h, seq, loop_count);
        stat_show++;
    }

    /* 停止时把还没处理的 pending 还掉（否则那个槽要等 App 进程退出才回来）。*/
    tick_stop();

    if (have_pending) {
        return_unsubmitted(pending.id);
        have_pending = false;
    }
    LOGI("render loop stopped");
    return NULL;
}

/* ======================== audio ========================
 * /dev/vdrm-audio 的共享环由 vdrm.ko 的 ALSA playback 填充；
 * 这里 mmap 环、poll 可读、把 PCM 写进 AAudio。
 */

#define AUDIO_FRAME_BYTES (VDRM_AUDIO_CHANNELS * 2)   /* S16_LE 立体声 */
#define AUDIO_WRITE_TIMEOUT_NS 50000000LL             /* 50ms：别把环堵满 */
/* 一次最多搬 ~20ms：块太大时 AAudio 会攒着写，tail 更新变粗，ALSA 那边
 * 看到的就是一阵一阵的消费，延迟也跟着变大。 */
#define AUDIO_CHUNK_MAX (VDRM_AUDIO_RATE / 50 * AUDIO_FRAME_BYTES)

static void *audio_play_thread(void *arg)
{
    struct consumer *c = arg;
    struct vdrm_audio_ring *r = c->audio_map;
    int err_logged = 0;

    if (!r)
        return NULL;
    uint32_t gen = __atomic_load_n(&r->generation, __ATOMIC_ACQUIRE);
    while (c->audio_running) {
        struct pollfd p = { .fd = c->audio_fd, .events = POLLIN };
        uint32_t head, tail, avail, off, n;
        uint32_t newgen;
        int32_t written;

        if (poll(&p, 1, 100) <= 0)
            continue;
        /* 内核 prepare 抬了代次：丢掉本地旧 tail，从新 head 重新同步。 */
        newgen = __atomic_load_n(&r->generation, __ATOMIC_ACQUIRE);
        if (newgen != gen) {
            gen = newgen;
            head = __atomic_load_n(&r->head, __ATOMIC_ACQUIRE);
            __atomic_store_n(&r->tail, head, __ATOMIC_RELEASE);
            ioctl(c->audio_fd, VDRM_AUDIO_KICK);
            continue;
        }
        head = __atomic_load_n(&r->head, __ATOMIC_ACQUIRE);
        tail = __atomic_load_n(&r->tail, __ATOMIC_RELAXED);
        avail = head - tail;
        /* 内核 prepare 会把 head/tail 清零。若本线程正好有一次写还没落地，
         * tail 会被甩到 head 前面，avail 变成巨大的回绕值。环里永远不可能
         * 有超过 size 的数据，所以这一定是重同步，不是真数据。 */
        if (avail > r->size) {
            __atomic_store_n(&r->tail, head, __ATOMIC_RELEASE);
            /* 不踢一下内核，它不会推 hw_ptr，容器里的写者会在 10s 后拿 -EIO。*/
            ioctl(c->audio_fd, VDRM_AUDIO_KICK);
            continue;
        }
        if (!avail)
            continue;
        off = tail % r->size;
        n = avail < r->size - off ? avail : r->size - off;
        if (n > AUDIO_CHUNK_MAX)
            n = AUDIO_CHUNK_MAX;

        /* 只把真正写进 AAudio 的帧算作消费：超时/短写时 tail 不动，
         * 下一轮重试，不能像以前那样无条件把整段 avail 记成已消费。 */
        written = AAudioStream_write(c->play_stream, r->data + off,
                                     n / AUDIO_FRAME_BYTES,
                                     AUDIO_WRITE_TIMEOUT_NS);
        if (written > 0) {
            __atomic_store_n(&r->tail,
                             tail + (uint32_t)written * AUDIO_FRAME_BYTES,
                             __ATOMIC_RELEASE);
            /* 告诉内核"又消费了一段"：它要推 hw_ptr 并叫醒容器里等空间的
             * 写者，否则对方会在 10 秒后拿到 -EIO。 */
            ioctl(c->audio_fd, VDRM_AUDIO_KICK);
            err_logged = 0;
            continue;
        }
        if (written == AAUDIO_ERROR_TIMEOUT || written == 0)
            continue;              /* 只是暂时没空间，不是流坏了 */
        if (!err_logged) {
            LOGE("audio: AAudioStream_write err=%d，丢弃积压", written);
            err_logged = 1;
        }
        /* 流没了（耳机拔掉/被抢占）：丢掉积压，别让环一直满着空转 */
        __atomic_store_n(&r->tail, head, __ATOMIC_RELEASE);
        ioctl(c->audio_fd, VDRM_AUDIO_KICK);
        usleep(10000);
    }
    return NULL;
}

static int audio_ring_open(struct consumer *c)
{
    struct vdrm_audio_info info;
    void *map;
    int fd;

    if (c->audio_fd >= 0)
        return 0;
    fd = open("/dev/vdrm-audio", O_RDWR | O_CLOEXEC);
    if (fd < 0) {
        LOGE("audio: open /dev/vdrm-audio: %s", strerror(errno));
        return -1;
    }
    if (ioctl(fd, VDRM_AUDIO_GET_INFO, &info) < 0 ||
        info.magic != VDRM_AUDIO_MAGIC) {
        LOGE("audio: GET_INFO failed");
        close(fd);
        return -1;
    }
    map = mmap(NULL, info.header_size + info.size, PROT_READ | PROT_WRITE,
               MAP_SHARED, fd, 0);
    if (map == MAP_FAILED) {
        LOGE("audio: mmap failed: %s", strerror(errno));
        close(fd);
        return -1;
    }
    c->audio_fd = fd;
    c->audio_map = map;
    c->audio_map_len = info.header_size + info.size;
    LOGI("audio: ring opened %u Hz %u ch, ring %u (idle until PCM opens)",
         info.rate, info.channels, info.size);
    return 0;
}

static void audio_ring_close(struct consumer *c)
{
    if (c->audio_map) {
        munmap(c->audio_map, c->audio_map_len);
        c->audio_map = NULL;
    }
    if (c->audio_fd >= 0) {
        close(c->audio_fd);
        c->audio_fd = -1;
    }
    c->audio_map_len = 0;
}

static void audio_play_stop(struct consumer *c)
{
    if (c->audio_running) {
        c->audio_running = false;
        pthread_join(c->play_thr, NULL);
    }
    if (c->play_stream) {
        AAudioStream_requestStop(c->play_stream);
        AAudioStream_close(c->play_stream);
        c->play_stream = NULL;
        LOGI("audio: AAudio output stopped (PCM closed/idle)");
    }
}

static int audio_cap_start(struct consumer *c);
static void audio_cap_stop(struct consumer *c);

static int audio_start(struct consumer *c)
{
    struct vdrm_audio_info info;
    AAudioStreamBuilder *b = NULL;
    AAudioStream *s = NULL;
    int rc;

    if (c->audio_running)
        return 0;
    if (audio_ring_open(c) < 0)
        return -1;
    if (ioctl(c->audio_fd, VDRM_AUDIO_GET_INFO, &info) < 0 ||
        info.magic != VDRM_AUDIO_MAGIC) {
        LOGE("audio: GET_INFO failed");
        return -1;
    }
    if (AAudio_createStreamBuilder(&b) != AAUDIO_OK)
        return -1;
    AAudioStreamBuilder_setDirection(b, AAUDIO_DIRECTION_OUTPUT);
    AAudioStreamBuilder_setPerformanceMode(b, AAUDIO_PERFORMANCE_MODE_LOW_LATENCY);
    AAudioStreamBuilder_setFormat(b, AAUDIO_FORMAT_PCM_I16);
    AAudioStreamBuilder_setSampleRate(b, info.rate);
    AAudioStreamBuilder_setChannelCount(b, info.channels);
    AAudioStreamBuilder_setUsage(b, AAUDIO_USAGE_MEDIA);
    AAudioStreamBuilder_setContentType(b, AAUDIO_CONTENT_TYPE_MUSIC);
    if (AAudioStreamBuilder_openStream(b, &s) != AAUDIO_OK) {
        LOGE("audio: openStream failed");
        AAudioStreamBuilder_delete(b);
        return -1;
    }
    AAudioStreamBuilder_delete(b);
    if (AAudioStream_requestStart(s) != AAUDIO_OK) {
        AAudioStream_close(s);
        return -1;
    }

    c->play_stream = s;
    c->audio_running = true;
    rc = pthread_create(&c->play_thr, NULL, audio_play_thread, c);
    if (rc != 0) {
        c->audio_running = false;
        AAudioStream_close(s);
        c->play_stream = NULL;
        return -1;
    }
    LOGI("audio: AAudio output started %u Hz %u ch", info.rate, info.channels);
    return 0;
}

static void *audio_monitor_thread(void *arg)
{
    struct consumer *c = arg;
    uint64_t last_play = 0;

    while (c->audio_mon_running) {
        if (c->audio_ev_fd >= 0) {
            struct pollfd p = { .fd = c->audio_ev_fd, .events = POLLIN };
            uint64_t v;

            if (poll(&p, 1, 1000) > 0)
                while (read(c->audio_ev_fd, &v, sizeof(v)) > 0) { }
        } else {
            usleep(500000);
        }

        struct vdrm_audio_state st;
        if (ioctl(c->audio_fd, VDRM_AUDIO_GET_STATE, &st) < 0) {
            if (!c->audio_legacy) {
                c->audio_legacy = true;
                LOGW("audio: old module (no GET_STATE), fallback: playback on, capture off");
                audio_start(c);
                /* 旧模块没有 capture 计数，无法按需；这里绝不常开麦克风。
                 * 容器录音要等新模块加载后再启用。 */
            }
            continue;
        }

        uint64_t now = now_ms();

        if (st.play_users) {
            last_play = now;
            if (!c->audio_running)
                audio_start(c);
        } else if (c->audio_running && now - last_play > 2000) {
            audio_play_stop(c);
        }

        if (st.cap_users) {
            if (!c->cap_running)
                audio_cap_start(c);
        } else if (c->cap_running) {
            audio_cap_stop(c);
        }
    }
    return NULL;
}

static int audio_monitor_start(struct consumer *c)
{
    int ev;

    if (c->audio_mon_running)
        return 0;
    if (audio_ring_open(c) < 0)
        return -1;

    ev = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (ev >= 0) {
        if (ioctl(c->audio_fd, VDRM_AUDIO_SET_EVENTFD, &ev) < 0) {
            LOGW("audio: SET_EVENTFD failed (%s), polling", strerror(errno));
            close(ev);
            ev = -1;
        }
    }
    c->audio_ev_fd = ev;
    c->audio_legacy = false;
    c->audio_mon_running = true;
    if (pthread_create(&c->audio_mon_thr, NULL, audio_monitor_thread, c) != 0) {
        c->audio_mon_running = false;
        if (c->audio_ev_fd >= 0) {
            close(c->audio_ev_fd);
            c->audio_ev_fd = -1;
        }
        return -1;
    }
    LOGI("audio: monitor started (output/input on demand)");
    return 0;
}

static void audio_monitor_stop(struct consumer *c)
{
    if (!c->audio_mon_running)
        return;
    c->audio_mon_running = false;
    if (c->audio_ev_fd >= 0) {
        uint64_t one = 1;
        ssize_t r = write(c->audio_ev_fd, &one, sizeof(one));
        (void)r;
    }
    pthread_join(c->audio_mon_thr, NULL);
    if (c->audio_ev_fd >= 0) {
        close(c->audio_ev_fd);
        c->audio_ev_fd = -1;
    }
}

static void audio_stop(struct consumer *c)
{
    audio_monitor_stop(c);
    audio_play_stop(c);
    audio_cap_stop(c);
    audio_ring_close(c);
}

/* 采集：AAudio INPUT 录到的帧 -> /dev/vdrm-audio-in 的共享环。
 *
 * 内核那条 capture PCM 的 hw_ptr 就是"App 累计写了多少帧"，所以这里只做三件
 * 事：读麦克风、按 tail 算剩余空间写进环、每段 ioctl(KICK) 叫醒容器里的读者。
 * 环满就丢这一段 —— 宁可丢麦克风数据也不能把环写爆。*/
static void *audio_cap_thread(void *arg)
{
    struct consumer *c = arg;
    struct vdrm_audio_ring *r = c->cap_map;
    int16_t buf[2048 * VDRM_AUDIO_CHANNELS];
    const uint32_t fb = VDRM_AUDIO_CHANNELS * 2;

    if (!r)
        return NULL;

    while (c->cap_running) {
        int32_t n = AAudioStream_read(c->cap_stream, buf, 1024, 20000000); /* 20ms */
        uint32_t head, tail, size, used, bytes, space, off, first;

        if (n <= 0) {
            if (n == AAUDIO_ERROR_TIMEOUT)
                continue;
            LOGW("audio-in: AAudioStream_read err=%d", n);
            usleep(20000);
            continue;
        }

        size  = r->size;
        head  = __atomic_load_n(&r->head, __ATOMIC_RELAXED);
        tail  = __atomic_load_n(&r->tail, __ATOMIC_ACQUIRE);
        used  = head - tail;              /* 两者都在 [0,size) 内，无符号差正确 */
        if (used >= size) {
            /* tail 还没跟上：环满，丢这一段 */
            continue;
        }
        bytes = (uint32_t)n * fb;
        space = size - used;
        if (bytes > space) {
            /* 只写放得下的部分，剩下的丢掉 */
            n = (int32_t)(space / fb);
            bytes = (uint32_t)n * fb;
            if (n <= 0)
                continue;
        }

        off   = head % size;
        first = (bytes < size - off) ? bytes : size - off;
        memcpy(r->data + off, buf, first);
        if (bytes > first)
            memcpy(r->data, (const uint8_t *)buf + first, bytes - first);

        head = (head + bytes) % size;
        __atomic_store_n(&r->head, head, __ATOMIC_RELEASE);
        ioctl(c->cap_fd, VDRM_AUDIO_KICK);
    }
    return NULL;
}

static int audio_cap_start(struct consumer *c)
{
    struct vdrm_audio_info info;
    AAudioStreamBuilder *b = NULL;
    AAudioStream *s = NULL;
    void *map;
    int fd;

    if (c->cap_running)
        return 0;
    fd = open("/dev/vdrm-audio-in", O_RDWR | O_CLOEXEC);
    if (fd < 0) {
        LOGW("audio-in: open /dev/vdrm-audio-in: %s", strerror(errno));
        return -1;
    }
    memset(&info, 0, sizeof(info));
    if (ioctl(fd, VDRM_AUDIO_GET_INFO, &info) < 0 ||
        info.magic != VDRM_AUDIO_MAGIC || !info.size) {
        LOGE("audio-in: GET_INFO failed");
        close(fd);
        return -1;
    }
    map = mmap(NULL, info.header_size + info.size, PROT_READ | PROT_WRITE,
               MAP_SHARED, fd, 0);
    if (map == MAP_FAILED) {
        LOGE("audio-in: mmap failed: %s", strerror(errno));
        close(fd);
        return -1;
    }
    if (AAudio_createStreamBuilder(&b) != AAUDIO_OK) {
        munmap(map, info.header_size + info.size);
        close(fd);
        return -1;
    }
    AAudioStreamBuilder_setDirection(b, AAUDIO_DIRECTION_INPUT);
    AAudioStreamBuilder_setPerformanceMode(b, AAUDIO_PERFORMANCE_MODE_LOW_LATENCY);
    AAudioStreamBuilder_setFormat(b, AAUDIO_FORMAT_PCM_I16);
    AAudioStreamBuilder_setSampleRate(b, info.rate);
    AAudioStreamBuilder_setChannelCount(b, info.channels);
    AAudioStreamBuilder_setInputPreset(b, AAUDIO_INPUT_PRESET_VOICE_RECOGNITION);
    if (AAudioStreamBuilder_openStream(b, &s) != AAUDIO_OK) {
        LOGE("audio-in: openStream failed（RECORD_AUDIO 权限给了吗？）");
        AAudioStreamBuilder_delete(b);
        munmap(map, info.header_size + info.size);
        close(fd);
        return -1;
    }
    AAudioStreamBuilder_delete(b);
    if (AAudioStream_requestStart(s) != AAUDIO_OK) {
        AAudioStream_close(s);
        munmap(map, info.header_size + info.size);
        close(fd);
        return -1;
    }

    c->cap_fd      = fd;
    c->cap_map     = map;
    c->cap_map_len = info.header_size + info.size;
    c->cap_stream  = s;
    c->cap_running = true;
    if (pthread_create(&c->cap_thr, NULL, audio_cap_thread, c) != 0) {
        c->cap_running = false;
        AAudioStream_close(s);
        c->cap_stream = NULL;
        munmap(map, info.header_size + info.size);
        close(fd);
        c->cap_fd = -1;
        c->cap_map = NULL;
        return -1;
    }
    LOGI("audio-in: capturing %u Hz %u ch ring=%u", info.rate, info.channels, info.size);
    return 0;
}

static void audio_cap_stop(struct consumer *c)
{
    if (c->cap_running) {
        c->cap_running = false;
        pthread_join(c->cap_thr, NULL);
    }
    if (c->cap_stream) {
        AAudioStream_requestStop(c->cap_stream);
        AAudioStream_close(c->cap_stream);
        c->cap_stream = NULL;
        LOGI("audio-in: capture stopped (PCM closed)");
    }
    if (c->cap_map) {
        munmap(c->cap_map, c->cap_map_len);
        c->cap_map = NULL;
    }
    if (c->cap_fd >= 0) {
        close(c->cap_fd);
        c->cap_fd = -1;
    }
    c->cap_map_len = 0;
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
    if (g_return_fd < 0)
        g_return_fd = fcntl(g_vdrm_fd, F_DUPFD_CLOEXEC, 0);
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

/* 一次 ioctl 发多个事件；内核每个设备只补一个 SYN。*/
static void in_emit_batch(const struct vdrm_input_event *ev, int n)
{
    struct vdrm_input_batch b;
    static int warned;

    if (n <= 0)
        return;
    if (n > VDRM_INPUT_BATCH_MAX)
        n = VDRM_INPUT_BATCH_MAX;
    if (g_vdrm_fd < 0 && vdrm_open_lazy() < 0) {
        if (!warned) {
            warned = 1;
            LOGW("input: %s 打不开 err=%d —— 事件被丢弃", VDRM_DEV, errno);
        }
        return;
    }
    warned = 0;
    memset(&b, 0, sizeof(b));
    b.count = (uint32_t)n;
    memcpy(b.events, ev, (size_t)n * sizeof(ev[0]));
    if (ioctl(g_vdrm_fd, VDRM_IOCTL_INPUT_BATCH, &b) < 0)
        LOGW("input: batch ioctl err=%d", errno);
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
    struct vdrm_input_event ev[2];
    int ix, iy, n = 0;

    g_in_mv[0] += dx;
    g_in_mv[1] += dy;
    ix = (int)g_in_mv[0];
    iy = (int)g_in_mv[1];
    if (ix) {
        g_in_mv[0] -= (float)ix;
        ev[n].type = IN_EV_REL; ev[n].code = IN_REL_X;
        ev[n].value = ix; n++;
    }
    if (iy) {
        g_in_mv[1] -= (float)iy;
        ev[n].type = IN_EV_REL; ev[n].code = IN_REL_Y;
        ev[n].value = iy; n++;
    }
    if (n)
        in_emit_batch(ev, n);
}

static void in_scroll(int axis, float v)
{
    int idx = axis ? 1 : 0;
    uint16_t rel = axis ? IN_REL_HWHEEL : IN_REL_WHEEL;

    /* 自然滚动（内容跟手）：Java 侧按手指位移方向发值，而 evdev 的
     * REL_WHEEL/REL_HWHEEL 正方向与手指方向相反（+1 = 滚轮上/右滚），
     * 所以这里统一取反再折算档位 —— 横向、纵向都翻，方向才是对的。*/
    g_in_sc[idx] -= v;
    while (g_in_sc[idx] >= IN_SCROLL_PX_PER_NOTCH) {
        g_in_sc[idx] -= IN_SCROLL_PX_PER_NOTCH;
        in_emit(IN_EV_REL, rel, 1);
    }
    while (g_in_sc[idx] <= -IN_SCROLL_PX_PER_NOTCH) {
        g_in_sc[idx] += IN_SCROLL_PX_PER_NOTCH;
        in_emit(IN_EV_REL, rel, -1);
    }
}

/* ---- 直接触摸：窗口坐标 → mode 像素 → 0..4095 归一化 ----
 * 和 sc_set_geometry 的 letterbox 用同一套公式；黑边区域的触点直接丢弃。
 * 这些变量只在 Java UI 线程更新/读取（nativeStart / onTouchEvent）。*/
static int g_touch_mw, g_touch_mh;   /* 当前 mode */
static int g_touch_ww, g_touch_wh;   /* 当前窗口 */

static void touch_update_geometry(int mw, int mh, int ww, int wh)
{
    g_touch_mw = mw;
    g_touch_mh = mh;
    g_touch_ww = ww;
    g_touch_wh = wh;
}

static int touch_map_xy(int wx, int wy, int *ox, int *oy)
{
    int mw = g_touch_mw, mh = g_touch_mh, ww = g_touch_ww, wh = g_touch_wh;
    int rot, dw, dh, dx, dy, pw, ph, px, py;

    if (mw <= 0 || mh <= 0 || ww <= 0 || wh <= 0)
        return -1;

    /* 摆放和 sc_set_geometry 用同一个函数算：旋转/缩放/居中一定对得上。*/
    {
        int32_t ldw, ldh, ldx, ldy;

        present_layout((uint32_t)mw, (uint32_t)mh, (uint32_t)ww, (uint32_t)wh,
                       &rot, &ldw, &ldh, &ldx, &ldy);
        dw = ldw; dh = ldh; dx = ldx; dy = ldy;
    }
    /* 画面（旋转之后）的像素尺寸 */
    pw = (rot == VDRM_ROT_NONE) ? mw : mh;
    ph = (rot == VDRM_ROT_NONE) ? mh : mw;

    /* 落在黑边不丢点：夹到画面区边缘。内核会把"这一帧没出现的 slot"当成
     * 抬起，直接丢掉会让拖拽进入黑边时断指。*/
    if (wx < dx) wx = dx;
    if (wx >= dx + dw) wx = dx + dw - 1;
    if (wy < dy) wy = dy;
    if (wy >= dy + dh) wy = dy + dh - 1;

    /* 窗口坐标 → 画面（旋转后）像素坐标 */
    px = (int)(((double)(wx - dx) * (double)pw / (double)dw) + 0.5);
    py = (int)(((double)(wy - dy) * (double)ph / (double)dh) + 0.5);
    if (px < 0) px = 0;
    if (py < 0) py = 0;
    if (px > pw - 1) px = pw - 1;
    if (py > ph - 1) py = ph - 1;

    /* 再按旋转的逆变换回到 buffer 像素坐标。
     * ROT_90（顺时针显示）下：显示(sx,sy) = (mh-1-by, bx)
     *   → bx = sy, by = mh-1-sx */
    if (rot == VDRM_ROT_90) {
        int bx = py;
        int by = mh - 1 - px;
        px = bx;
        py = by;
    } else if (rot == VDRM_ROT_270) {
        int bx = mw - 1 - py;
        int by = px;
        px = bx;
        py = by;
    }

    *ox = (int)((double)px * VDRM_TOUCH_COORD_MAX /
                (double)(mw > 1 ? mw - 1 : 1) + 0.5);
    *oy = (int)((double)py * VDRM_TOUCH_COORD_MAX /
                (double)(mh > 1 ? mh - 1 : 1) + 0.5);
    if (*ox < 0) *ox = 0;
    if (*ox > VDRM_TOUCH_COORD_MAX) *ox = VDRM_TOUCH_COORD_MAX;
    if (*oy < 0) *oy = 0;
    if (*oy > VDRM_TOUCH_COORD_MAX) *oy = VDRM_TOUCH_COORD_MAX;
    return 0;
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

/* 给 Java 的"配置的 KPM 输出几何"：返回 (宽 << 16) | 高，拿不到返回 0。
 *
 * Java 拿它强制 Activity 的横竖（竖配置 → 竖窗口）。优先用 GET_CONF：那是
 * mode 参数（配置值），切换分辨率后立刻生效，不用等合成器真的切过去。
 * 老模块没有 GET_CONF 时退回 GET_INFO（合成器在用的尺寸），至少方向是对的。*/
JNIEXPORT void JNICALL
Java_com_vdrm_consumer_Native_nativeSetInflight(JNIEnv *env, jclass clazz, jint n)
{
	g_inflight_override = (n >= 1 && n <= 6) ? n : -1;
}

JNIEXPORT void JNICALL
Java_com_vdrm_consumer_Native_nativeSetPll(JNIEnv *env, jclass clazz, jboolean on)
{
	g_pll_enabled = on ? 1 : 0;
}

JNIEXPORT jint JNICALL
Java_com_vdrm_consumer_Native_nativeGetMode(JNIEnv *env, jclass clazz)
{
    struct vdrm_conf conf;
    struct vdrm_info info;
    static uint32_t seen_commit;
    static uint32_t pending_w, pending_h;
    static uint64_t pending_since_ms;
    static int commit_init;
    int have_conf, have_info;

    (void)env; (void)clazz;
    if (vdrm_open() != 0)
        return 0;

    /* commit 一发生就用目标尺寸转窗口，让 Android 的旋转动画和合成器换档
     * 重叠；在 GET_INFO（g_active）追上之前一直用 pending，避免下一轮
     * 回退成旧方向。没有 commit（只写 mode）时仍然跟 g_active。*/
    memset(&conf, 0, sizeof(conf));
    have_conf = (ioctl(g_vdrm_fd, VDRM_IOCTL_GET_CONF, &conf) == 0 &&
                 conf.width && conf.height);
    if (have_conf) {
        if (!commit_init) {
            /* 进程启动时内核里可能已经有过 commit（上一次切档留下的）。
             * 那不代表“App 运行期间发生了新切档”，不能拿它的目标尺寸去
             * 转屏 —— 否则合成器已经横屏、目标参数还是竖屏时，App 会
             * 一启动就竖着，把横屏帧缩进竖窗口。首次只记基线。*/
            commit_init = 1;
            seen_commit = conf.commit_gen;
        } else if (conf.commit_gen != seen_commit) {
            seen_commit = conf.commit_gen;
            pending_w = conf.width;
            pending_h = conf.height;
            pending_since_ms = now_ms();
        }
    }

    memset(&info, 0, sizeof(info));
    have_info = (vdrm_get_info(&info) == 0 && info.width && info.height);
    if (pending_w) {
        if (have_info && info.width == pending_w && info.height == pending_h) {
            pending_w = pending_h = 0;
        } else if (now_ms() - pending_since_ms > 1000) {
            /* commit 后一秒 g_active 还没对上：合成器很可能选了别的档
             * （或者干脆没换），跟实际 active，别把窗口钉死在旧目标上。*/
            pending_w = pending_h = 0;
        }
    }

    if (pending_w)
        return (jint)(((pending_w & 0xffffu) << 16) | (pending_h & 0xffffu));
    if (have_info)
        return (jint)(((info.width & 0xffffu) << 16) | (info.height & 0xffffu));
    if (have_conf)
        return (jint)(((conf.width & 0xffffu) << 16) | (conf.height & 0xffffu));
    return 0;
}

/* 直接触摸：Java 每帧把当前活动触点打包传进来（空 count = 全部抬起）。
 * ids/xs/ys/ps 长度一致；ids 是 pointerId，作为 slot/tracking 的稳定身份。*/
JNIEXPORT jint JNICALL
Java_com_vdrm_consumer_Native_nativeSendTouch(JNIEnv *env, jclass clazz,
                                              jint count, jintArray jids,
                                              jfloatArray jxs, jfloatArray jys,
                                              jfloatArray jps)
{
    struct vdrm_touch_frame f;
    jint *ids = NULL;
    jfloat *xs = NULL, *ys = NULL, *ps = NULL;
    int i, n = (int)count;
    int ret = 0;

    (void)clazz;
    if (n < 0) n = 0;
    if (n > VDRM_TOUCH_MAX) n = VDRM_TOUCH_MAX;

    memset(&f, 0, sizeof(f));
    if (n > 0) {
        jsize li = (*env)->GetArrayLength(env, jids);
        jsize lx = (*env)->GetArrayLength(env, jxs);
        jsize ly = (*env)->GetArrayLength(env, jys);
        jsize lp = (*env)->GetArrayLength(env, jps);

        if (n > li) n = (int)li;
        if (n > lx) n = (int)lx;
        if (n > ly) n = (int)ly;
        if (n > lp) n = (int)lp;
        if (n > 0) {
            ids = (*env)->GetIntArrayElements(env, jids, NULL);
            xs  = (*env)->GetFloatArrayElements(env, jxs, NULL);
            ys  = (*env)->GetFloatArrayElements(env, jys, NULL);
            ps  = (*env)->GetFloatArrayElements(env, jps, NULL);
            if (!ids || !xs || !ys || !ps) {
                ret = -1;
                goto out;
            }
        }
    }

    {
    uint32_t used_slots = 0;

    for (i = 0; i < n; i++) {
        int mx, my, pr;
        uint32_t slot;

        if (touch_map_xy((int)(xs[i] + 0.5f), (int)(ys[i] + 0.5f), &mx, &my) < 0)
            continue;   /* 几何还没准备好 */
        /* ids 取模可能撞车：线性探测一个本帧没用过的 slot。 */
        slot = (uint32_t)(ids[i] % VDRM_TOUCH_MAX);
        while (used_slots & (1u << slot))
            slot = (slot + 1) % VDRM_TOUCH_MAX;
        used_slots |= 1u << slot;
        f.points[f.count].slot     = (int32_t)slot;
        f.points[f.count].tracking = (int32_t)ids[i];
        f.points[f.count].x        = mx;
        f.points[f.count].y        = my;
        pr = (int)(ps[i] * 255.0f + 0.5f);
        if (pr < 0) pr = 0;
        if (pr > 255) pr = 255;
        f.points[f.count].pressure = pr;
        f.count++;
    }
    }

    if (g_vdrm_fd < 0 && vdrm_open_lazy() < 0) {
        ret = -1;
        goto out;
    }
    if (ioctl(g_vdrm_fd, VDRM_IOCTL_TOUCH, &f) < 0) {
        LOGW("touch: ioctl err=%d", errno);
        ret = -1;
    }

out:
    if (ids) (*env)->ReleaseIntArrayElements(env, jids, ids, JNI_ABORT);
    if (xs)  (*env)->ReleaseFloatArrayElements(env, jxs, xs, JNI_ABORT);
    if (ys)  (*env)->ReleaseFloatArrayElements(env, jys, ys, JNI_ABORT);
    if (ps)  (*env)->ReleaseFloatArrayElements(env, jps, ps, JNI_ABORT);
    return ret;
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
    c->audio_fd = -1;
    c->cap_fd = -1;
    /* 进程级 release worker：只起一次，之后所有 frame 共用一个线程。*/
    pthread_once(&g_release_once, release_worker_init);
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
    audio_stop(c);
    sc_remove(g_sc);
    g_sc = NULL;
    present_flush();   /* 图层没了，等它的上屏信号也没意义 */
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

/* ---- 池按需创建（分辨率归合成器）----
 *
 * 合成器从广告列表里挑了某一档、但池里还没有该尺寸的槽时，内核会把目标尺寸
 * 写到 want_mode、把 want_gen 加一，并 eventfd 叫醒我们。这里看到 gen 变化
 * 就拉一次 `vdrm_mkpool --pool WxH`（只补槽，不改 mode、不 commit），
 * 合成器那边 dumb_create 的等待循环会因 REGISTER 被唤醒、正常拿到 buffer。*/
static unsigned int g_want_gen_seen;

static bool read_sysfs_str(const char *path, char *buf, size_t n)
{
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    ssize_t r;

    if (fd < 0)
        return false;
    r = read(fd, buf, n - 1);
    close(fd);
    if (r <= 0)
        return false;
    buf[r] = 0;
    while (r > 0 && (buf[r - 1] == '\n' || buf[r - 1] == '\r' ||
                     buf[r - 1] == ' '))
        buf[--r] = 0;
    return r > 0;
}

static void maybe_request_pool(void)
{
    struct vdrm_want w;
    char want_s[24], cmd[160];
    pid_t pid;

    if (g_vdrm_fd < 0)
        return;
    memset(&w, 0, sizeof(w));
    if (ioctl(g_vdrm_fd, VDRM_IOCTL_WANT, &w) < 0)
        return;
    if (w.gen == g_want_gen_seen || !w.width || !w.height)
        return;
    g_want_gen_seen = w.gen;
    snprintf(want_s, sizeof(want_s), "%ux%u", w.width, w.height);
    LOGI("pool-on-demand: want %s (gen=%u) -> mkpool --pool", want_s, w.gen);
    snprintf(cmd, sizeof(cmd), VDRM_MKPOOL " --pool %s", want_s);
    pid = fork();
    if (pid == 0) {
        execl("/system/bin/su", "su", "-c", cmd, (char *)NULL);
        _exit(127);
    }
}

static void ensure_pool_spawn(int force)
{
    struct vdrm_pool_list lst;
    int n, usable = 0, i;
    pid_t pid;

    n = vdrm_get_pool(&lst);
    for (i = 0; i < n; i++) {
        if (lst.items[i].fd >= 0) {
            usable++;
            close(lst.items[i].fd);
        }
        if (lst.items[i].fd1 >= 0)
            close(lst.items[i].fd1);
    }
    if (!force && usable > 0)
        return;   /* 有可用的真池（带原始 fd）就不管 */
    /* 池空，或只剩 HEAP 回退登记的“无 fd”槽：都要重建（mkpool 会先清池）。
     * force=1：mode 变了，旧池尺寸不对，必须重建。*/
    LOGI("ensure_pool: %s(n=%d usable=%d) -> 拉起 mkpool",
         force ? "强制重建" : "池不可用", n, usable);
    pid = fork();
    if (pid == 0) {
        execl("/system/bin/su", "su", "-c", VDRM_MKPOOL, (char *)NULL);
        _exit(127);
    }
}

/* 模块侧换了分辨率：把 App 这一侧的几何和池对上。
 *
 * 这里不再同步重试 10 秒。那条循环会把渲染线程（唯一消费帧事件、按 SF
 * release 回收槽的线程）堵死 10 秒，换档期间的尾帧事件全积在核心里；
 * 而且等不到池时只会打一行超时日志，屏幕还是黑的。
 *
 * 现在只做一次：更新 mode → 清旧 AHB → forge 一次。失败就返回，由渲染
 * 循环每 200ms 的周期检查重试；池真的空了由 ensure_pool_spawn 用 su 拉
 * mkpool 补（没有常驻 watcher 了）。
 *
 * 也不在这里 sc_blank()：旧帧按自己的 buffer 尺寸 letterbox 是正确的
 * （图层旋转已经去掉），先藏起来而新帧又没来，屏幕会一直黑。新尺寸的
 * 第一帧到达时 sc_latch 会自然替换画面。*/
static void maybe_rebuild_pool(struct consumer *c)
{
    static uint64_t last_forge_try_ms;
    struct vdrm_info info;

    if (g_vdrm_fd < 0)
        return;
    memset(&info, 0, sizeof(info));
    if (vdrm_get_info(&info) != 0 || !info.width || !info.height)
        return;

    if ((int)info.width == c->mode_w && (int)info.height == c->mode_h) {
        /* 尺寸没变，但池可能刚被补出来（App 启动早于 mkpool）：补 forge。 */
        if (c->buf_count == 0 &&
            now_ms() - last_forge_try_ms >= 1000) {
            last_forge_try_ms = now_ms();
            pthread_mutex_lock(&g_display_lock);
            if (forge_pool(c) == 0)
                LOGI("pool forged for %dx%d (%d bufs)",
                     c->mode_w, c->mode_h, c->buf_count);
            pthread_mutex_unlock(&g_display_lock);
        }
        return;
    }

    LOGI("runtime mode change %dx%d -> %ux%u, rebuilding pool",
         c->mode_w, c->mode_h, info.width, info.height);
    c->mode_w = (int)info.width;
    c->mode_h = (int)info.height;
    touch_update_geometry(c->mode_w, c->mode_h, c->screen_w, c->screen_h);
    pthread_mutex_lock(&g_geometry_lock);
    sc_geometry(c->mode_w, c->mode_h, c->screen_w, c->screen_h);
    pthread_mutex_unlock(&g_geometry_lock);

    pthread_mutex_lock(&g_display_lock);
    free_buffers(c);
    last_forge_try_ms = now_ms();
    if (forge_pool(c) == 0) {
        LOGI("pool rebuilt for %dx%d (%d bufs)",
             c->mode_w, c->mode_h, c->buf_count);
        vdrm_ready();   /* 换档那一帧多半已经丢了，让内核再推一次当前帧 */
    } else {
        LOGW("pool not ready for %dx%d; will retry", c->mode_w, c->mode_h);
    }
    pthread_mutex_unlock(&g_display_lock);
}

JNIEXPORT void JNICALL
Java_com_vdrm_consumer_Native_nativeStart(JNIEnv *env, jclass clazz,
                                          jlong handle, jobject surface)
{
    struct consumer *c = STATE(handle);
    if (!c) return;
    int same_surface = 0;   /* 纯 resize 时保留 buffer，不重 forge（避免卡在上一帧）*/
    int relatch_pending = 0; /* 新 SC 建好后要把上一帧贴回去（不然会黑一下）*/

    if (vdrm_open() < 0) {
        LOGE("vdrm_open failed (no /dev/vdrm access?)");
        return;
    }
    /* 缓存 JVM / Native 类，渲染线程收到 mode 事件时回调 Java。
     * Native 类只取一次；Activity 生命周期由 Java 侧的 listener 管。*/
    if (!g_jvm)
        (*env)->GetJavaVM(env, &g_jvm);
    if (!g_native_class) {
        g_native_class = (jclass)(*env)->NewGlobalRef(env, clazz);
        g_on_mode_changed = (*env)->GetStaticMethodID(env, clazz,
                                                      "onModeChanged", "()V");
        if (!g_on_mode_changed)
            (*env)->ExceptionClear(env);
    }
    /* 启动/换 surface 时立刻让 Java 按当前 active 方向对一次。
     * 不能只靠 onCreate 里的那次 sync：它在设备可访问/窗口建立之前就可能
     * 返回 0，或者窗口还没转过来，第一帧就会以默认竖屏被 letterbox。*/
    notify_mode_changed();
    /* 输出分辨率由模块的 mode 参数决定，App 不参与。窗口尺寸变化只改
     * letterbox 几何；不在这里 SET_MODE，避免运行中 mode 切换带来的
     * 池重建/热插拔边界状态。*/
    if (c->running && g_sc && g_last_surface &&
        (*env)->IsSameObject(env, g_last_surface, surface)) {
        pthread_mutex_lock(&g_geometry_lock);
        c->screen_w = ANativeWindow_getWidth(c->win);
        c->screen_h = ANativeWindow_getHeight(c->win);
        sc_geometry(c->mode_w, c->mode_h, c->screen_w, c->screen_h);
        touch_update_geometry(c->mode_w, c->mode_h, c->screen_w, c->screen_h);
        /* 窗口刚才转了方向（surface 没变、只是尺寸变了）：上一帧如果尺寸和
         * 当前 mode 一致，就按新的窗口几何重贴一遍。
         *
         * 为什么必须重贴：切分辨率时合成器往往只出一帧（然后就没 damage 了），
         * 那一帧常常在系统还没转完屏时就到了 —— 于是 rot 按"窗口还是竖的"算了
         * 90°，之后再也没有新帧把它改回来，屏幕上就一直是一张转错方向的画面。*/
        if (g_last_ahb && g_last_w == (uint32_t)c->mode_w &&
            g_last_h == (uint32_t)c->mode_h)
            sc_show_last((uint32_t)c->screen_w, (uint32_t)c->screen_h);
        pthread_mutex_unlock(&g_geometry_lock);
        /* 纯 resize 不会重建 SC，但同样需要让内核重推一次当前帧。*/
        vdrm_ready();
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
        /* Pure resize is handled above without stopping the render loop. */
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
            present_flush();   /* 旧图层的在飞帧：新图层上不会再有人等它们 */
            /* 新 SC 可能复用旧指针地址，几何/透明度缓存必须显式失效。 */
            pthread_mutex_lock(&g_geometry_lock);
            g_geom_sc = NULL;
            g_opaque_sc = NULL;
            g_fr_sc = NULL;
            pthread_mutex_unlock(&g_geometry_lock);
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
            relatch_pending = 1;
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

    /* 新 SC 建好、窗口尺寸也拿到了：立刻把上一帧贴回去。
     * 从"摘掉旧图层"到"渲染线程贴下一帧"之间本来是黑的，合成器空闲时这段
     * 还没有尽头 —— 看起来就是"切后台回来/息屏再亮，画面闪一下才出来"。*/
    if (relatch_pending)
        sc_show_last((uint32_t)c->screen_w, (uint32_t)c->screen_h);

    /* 不再上报窗口尺寸。
     *
     * 这两档"全面屏"分辨率现在由 mkpool 自己查（`wm size` 的 Physical/Override）
     * 再 SET_SCREEN 给模块。App 上报的是【窗口】尺寸：分屏/小窗时会比面板小，
     * 一上报就把面板那两档从 modes 里挤掉，合成器于是选不到 1264x2780 这种档，
     * 表现是黑屏或退回 16:9。窗口尺寸只影响 App 自己的 letterbox 几何，
     * 它从 GET_INFO 拿 mode、从 ANativeWindow 拿窗口大小，够用了。*/

    /* 几何从模块来（App 只跟随）；取不到就退回窗口尺寸。
     * 分辨率是模块 insmod 的 mode，不在运行时修改。*/

    {
        struct vdrm_info info;
        memset(&info, 0, sizeof(info));
        if (vdrm_get_info(&info) == 0 && info.width && info.height) {
            if (c->buf_count > 0 &&
                (c->mode_w != (int)info.width || c->mode_h != (int)info.height)) {
                LOGI("mode changed %dx%d -> %ux%u, rebuild buffers",
                     c->mode_w, c->mode_h, info.width, info.height);
                free_buffers(c);
                /* 旧帧是按旧几何贴上去的，先藏起来；新尺寸的帧一到就自动 SHOW。
                 * 同时清掉 last_buf 尺寸，免得 sc_geometry 继续拿旧 buffer 算。*/
                sc_blank();
                g_last_buf_w = 0;
                g_last_buf_h = 0;
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
        /* 触摸映射用同一套 mode/窗口几何 */
        touch_update_geometry(c->mode_w, c->mode_h, c->screen_w, c->screen_h);
    }

    /* A cached AHB may already have been returned; wait for a fresh frame. */

    /* Process-wide wake fd outlives asynchronous release callbacks. */
    if (g_release_wake_fd < 0)
        g_release_wake_fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (g_release_wake_fd < 0) {
        LOGE("release eventfd failed err=%d", errno);
        return;
    }

    /* 事件通道（KPM → us）。以前还有一条 shm 通道，内核早就没有对应
     * ioctl 了（App 侧一直拿到 ENOTTY），帧信息改由事件 + PRESENTED 上报
     * 承载，整条 mmap 路径已删除。*/
    c->event_fd = eventfd(0, EFD_NONBLOCK);
    if (c->event_fd < 0) {
        LOGE("eventfd failed err=%d", errno);
        return;
    }
    LOGI("registering event channel: event_fd=%d", c->event_fd);
    {
        int efd = c->event_fd;
        int r1 = ioctl(g_vdrm_fd, VDRM_IOCTL_SET_EVENTFD, &efd);
        LOGI("SET_EVENTFD=%d", r1);
    }

    /* B/A′：池由用户态 helper 在 KPM 加载后建好，App 只负责【重建 + 上屏】。
     * 纯 resize 时 buffer/池都没变 → 不 free/不重 forge，保住 g_last_ahb，
     * 下面用新几何重贴最后一帧 → 不会“卡在上一帧”。*/
    /* 虚拟键鼠：设备由 vdrm.ko 在内核注册，节点就在 /dev/input/eventN，
     * 由内核/ueventd 管理（注册时建、注销时删）。容器直接按节点绑定即可，
     * 不再需要 App 搬运 —— 也就不会留下悬空节点。*/

    /* 缓冲区是池里的（gralloc 内存 + 内核槽），跟 Surface 是谁无关：
     * Surface 重建时**不要** free/re-forge —— 那会把池槽全部 UNREGISTER 掉
     * 再让 mkpool 重新分配，期间新图层没东西可贴（黑一下），而且每次切
     * 后台回来都要重来一遍。只有池空（或尺寸变了，见 maybe_rebuild_pool）
     * 才重建。*/
    if (c->buf_count <= 0) {
        /* 池由模块 boot 时按固定 mode 建好；这里只在池不可用时兜底拉起。*/
        ensure_pool_spawn(0);
        free_buffers(c);
        if (forge_pool(c) < 0)
            LOGE("forge_pool 暂不可用（池未就绪？）— 渲染循环会自动重试");
    } else {
        LOGI("surface started -> keep %d buffers (no re-forge)", c->buf_count);
    }

    /* Start the render loop. */
    c->running = true;
    pthread_create(&c->thread, NULL, render_loop, c);


    LOGI("started (mode %dx%d) win %dx%d %d bufs",
         c->mode_w, c->mode_h, c->screen_w, c->screen_h, c->buf_count);

    /* 回前台的 onResume 往往先于 surfaceChanged：那时 nativeSetReady 只打出
     * "no layer yet"，READY 的帧被丢掉。Surface 真正 start 完必须再补一发，
     * 否则静态桌面没有新 damage，App 就停在最后一帧上（"卡住"）。*/
    vdrm_ready();
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
        int zero = 0;
        ioctl(g_vdrm_fd, VDRM_IOCTL_SET_EVENTFD, &zero);
    }

    sc_remove(g_sc);
    g_sc = NULL;
    present_flush();   /* 还挂着的 present fence 收掉，别指望它会 signal 了 */
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
    /* 只开 ring + monitor；AAudio 输出/采集由 monitor 按容器 PCM open 启停。 */
    if (audio_monitor_start(c) < 0)
        LOGE("audio: monitor start failed");
}

JNIEXPORT void JNICALL
Java_com_vdrm_consumer_Native_nativeStopAudio(JNIEnv *env, jclass clazz, jlong handle)
{
    struct consumer *c = STATE(handle);
    if (!c) return;
    audio_stop(c);
}
/* Java 侧 onPause 调的：App 进后台。
 *
 * 后台时 SF 不再消费我们的图层，继续 latch 只会让 submission/held/present
 * 三张表越攒越多；每帧都 RETURN 才是干净状态。present fence 全部 flush，
 * 等 release 的槽立即归还，拷贝目标环也放掉。回前台 nativeSetReady 会
 * 重新 READY + 重贴最后一帧。*/
JNIEXPORT void JNICALL
Java_com_vdrm_consumer_Native_nativeSetPaused(JNIEnv *env, jclass clazz,
                                              jlong handle, jboolean paused)
{
    (void)env; (void)clazz; (void)handle;

    if (paused) {
        atomic_store(&g_paused, true);
        present_flush();
        held_expire_force(true);
        LOGI("paused: flush present/held");
    } else {
        atomic_store(&g_paused, false);
        LOGI("resumed: render allowed");
    }
}

/* Java 侧 onResume 调的：App 回到前台，"就绪"。
 *
 * 后台时 SF 不再合成我们这层 → present 不来 → 内核的帧事件只能等存活超时
 * （VDRM_FLIP_FALLBACK_MS），桌面掉到个位数帧率。这时候如果干等合成器
 * 下一次提交，屏幕上会先停一下再醒过来 —— 那就是肉眼看到的"切回来卡一下"。
 *
 * 所以两件事一起做：
 *   1) 通知内核把当前帧立刻重推一次（不等合成器提交）；
 *   2) 自己把手里的最后一帧重贴回 SC 图层（后台期间那块可能是空的/旧的）。*/
JNIEXPORT void JNICALL
Java_com_vdrm_consumer_Native_nativeSetReady(JNIEnv *env, jclass clazz, jlong handle)
{
    struct consumer *c = STATE(handle);
    int32_t one = 1;

    (void)env; (void)clazz;
    if (vdrm_open() != 0)
        return;

    /* 回前台：先退出暂停/静默，把后台期间攒下的"等 release"槽全部还给内核，
     * 再 READY —— 顺序反了的话，内核可能先推来一帧又被 DISPL 槽卡住。*/
    atomic_store(&g_paused, false);
    held_expire_force(true);

    /* 老模块没有这个 ioctl → ENOTTY，当没事发生（画面靠下一帧自然恢复）。*/
    if (ioctl(g_vdrm_fd, VDRM_IOCTL_READY, &one) < 0 && errno != ENOTTY &&
        errno != EINVAL)
        LOGW("READY ioctl err=%d", errno);

    if (c && g_sc) {
        pthread_mutex_lock(&g_geometry_lock);
        sc_show_last((uint32_t)c->screen_w, (uint32_t)c->screen_h);
        pthread_mutex_unlock(&g_geometry_lock);
        LOGI("ready: ioctl + re-latch last frame");
    } else {
        LOGI("ready: ioctl (no layer yet)");
    }
}
