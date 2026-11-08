package com.vdrm.consumer;

import android.app.Activity;
import android.content.Intent;
import android.content.pm.PackageManager;
import android.content.res.Configuration;
import android.content.pm.ActivityInfo;
import android.os.Build;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.util.Log;
import android.view.KeyEvent;
import android.view.MotionEvent;
import android.view.SurfaceHolder;
import android.view.SurfaceView;
import android.view.View;
import android.view.WindowManager;
import android.widget.RelativeLayout;

public class MainActivity extends Activity implements SurfaceHolder.Callback {

    private static final String TAG = "VDRM";

    /* 输入模式：手机触摸要当成什么注入给容器。
     *
     *   0 = 鼠标    手指拖动 → REL 相对位移（VirtualTouchpad，触控板手感）
     *   1 = 触控屏  手指按哪就点哪 → ABS_MT 绝对坐标（经内核 vdrm-touch）
     *
     * 运行时可切：通知里的 `input 0` / `input 1` 会改这个静态字段并写进
     * SharedPreferences，下一次触摸立即按新模式走（不用重启 App）。*/
    public static final int INPUT_MOUSE = 0;
    public static final int INPUT_TOUCH = 1;
    public static final String PREFS = "vdrm";
    public static final String PREF_INPUT = "input_mode";

    /** volatile：通知服务（另一个线程）改，UI 线程每次触摸读 */
    public static volatile int inputMode = INPUT_TOUCH;

    private SurfaceView surfaceView;
    private VirtualTouchpad touchpad;
    private VirtualKeyboardView keyboardView;
    private long nativeHandle;
    /* 以前这里还持有一个 SCREEN_BRIGHT + ACQUIRE_CAUSES_WAKEUP 的唤醒锁。
     * 那个锁有两个副作用：
     *   1) ACQUIRE_CAUSES_WAKEUP 是"每次申请都把屏幕点亮"——进程被杀后前台服务
     *      重新拉起、Activity 重建，都会在息屏状态下把屏幕点亮，看起来像
     *      "电源键没生效/要按两次"；
     *   2) 常驻持有会让屏幕永不因超时熄灭。
     * 屏幕常亮这件事 window 上的 FLAG_KEEP_SCREEN_ON 已经做了，而且作用域只跟着
     * 窗口走（退到后台自动释放），不会主动点亮屏幕。所以这里不再要唤醒锁。*/
    private long lastBackTime;

    /* ---- 跟着虚拟屏的方向转 ----
     *
     * KPM 的输出是一块"显示器"，横竖由合成器选的 mode 决定（1080x1920 就是竖的）。
     * 手机这边窗口方向如果写死 landscape，竖屏 mode 就只能在中间缩成一小条 ——
     * 所以这里按 mode 的宽高申请窗口方向：竖 mode → 竖窗口，横 mode → 横窗口。
     * 这样虚拟屏才能按原比例尽量铺满屏幕（比例不同时仍由 native 的 letterbox
     * 补黑边，绝不拉伸）。
     *
     * 正常由内核的 mode eventfd 推事件，native 回调 onModeChanged()；
     * 1 秒的 Handler 只是兜底（事件丢失/老模块没有事件通道时仍能自愈）。*/
    private static final long ORIENT_POLL_MS = 1000;
    private int appliedOrientation;
    private final Handler orientHandler = new Handler(Looper.getMainLooper());
    private final Runnable orientPoll = new Runnable() {
        @Override
        public void run() {
            syncOrientation();

        /* 应用调试页保存的配置。pace 需要 /dev/vdrm 的 fd，这里先设一次
         * inflight/pll（纯 native 状态）；surfaceCreated 里再补一次 pace。*/
        VdrmSettingsActivity.applySaved(this);
            orientHandler.postDelayed(this, ORIENT_POLL_MS);
        }
    };

    /* 复用触摸数组，避免每个 MotionEvent 分配 4 个数组（120Hz 下很可观）。*/
    private static final int TOUCH_MAX = 10;   /* = VDRM_TOUCH_MAX */
    private final int[] touchIds = new int[TOUCH_MAX];
    private final float[] touchXs = new float[TOUCH_MAX];
    private final float[] touchYs = new float[TOUCH_MAX];
    private final float[] touchPs = new float[TOUCH_MAX];

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);

        /* 先按当前生效尺寸申请窗口方向，再 setContentView。
         * 放在后面的话 SurfaceView 会先按手机默认方向（通常是竖）创建，
         * 第一帧横屏画面就会先被等比缩进竖窗口，等转屏完成才铺满。
         * nativeGetMode 首次调用只看 GET_INFO（active），不会被上一次
         * commit 的目标尺寸带偏。*/
        syncOrientation();

        /* Fullscreen */
        getWindow().setFlags(WindowManager.LayoutParams.FLAG_FULLSCREEN,
                             WindowManager.LayoutParams.FLAG_FULLSCREEN);
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
            getWindow().setDecorFitsSystemWindows(false);
        } else {
            getWindow().getDecorView().setSystemUiVisibility(
                View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY |
                View.SYSTEM_UI_FLAG_FULLSCREEN |
                View.SYSTEM_UI_FLAG_HIDE_NAVIGATION |
                View.SYSTEM_UI_FLAG_LAYOUT_STABLE |
                View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION |
                View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN);
        }

        /* Keep screen on */
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);

        /* 不再投 120Hz 的票。
         *
         * 这台是 LTPO 屏：SF 的 VRR 策略本来就会在 60/90/120 之间按内容切，
         * 每次切面板都要重配 gamma/boost，暗画面（容器桌面）上就是可见的一闪。
         * App 硬投 120 等于一直把策略往 120 拉，和空闲计时器/别的窗口打架，
         * 只会制造更多切换。anland 那边从不投票，帧率完全交给系统。
         * 详见 docs/anland-output-path.md。*/
        WindowManager.LayoutParams attrs = getWindow().getAttributes();
        if (Build.VERSION.SDK_INT >= 30) {
            /* 允许窗口铺满摄像头挖空区：1920x1080 模式仍由 SC letterbox 留黑边，
             * 2376x1080 模式则真正 1:1 铺满（挖空会压在桌面上）。 */
            attrs.layoutInDisplayCutoutMode =
                WindowManager.LayoutParams.LAYOUT_IN_DISPLAY_CUTOUT_MODE_ALWAYS;
        }
        getWindow().setAttributes(attrs);

        /* Foreground notification (prevents Android from killing us) */
        requestPermissions();
        startForegroundService();

        /* Layout: surface + keyboard overlay */
        RelativeLayout root = new RelativeLayout(this);
        root.setLayoutParams(new RelativeLayout.LayoutParams(
                RelativeLayout.LayoutParams.MATCH_PARENT,
                RelativeLayout.LayoutParams.MATCH_PARENT));

        surfaceView = new SurfaceView(this);
        surfaceView.getHolder().addCallback(this);
        root.addView(surfaceView, new RelativeLayout.LayoutParams(
                RelativeLayout.LayoutParams.MATCH_PARENT,
                RelativeLayout.LayoutParams.MATCH_PARENT));

        /* Touch overlay — sits on top of surface, transparent */
        View touchOverlay = new View(this) {
            @Override
            public boolean onTouchEvent(MotionEvent event) {
                if (inputMode == INPUT_TOUCH) return sendTouchFrame(event);
                if (touchpad != null) return touchpad.onTouch(event);
                return true;
            }
        };
        touchOverlay.setLayoutParams(new RelativeLayout.LayoutParams(
                RelativeLayout.LayoutParams.MATCH_PARENT,
                RelativeLayout.LayoutParams.MATCH_PARENT));
        root.addView(touchOverlay);

        /* Virtual keyboard View — hidden by default */
        keyboardView = new VirtualKeyboardView(this);
        keyboardView.setVisibility(View.GONE);
        RelativeLayout.LayoutParams kvLp = new RelativeLayout.LayoutParams(
                RelativeLayout.LayoutParams.WRAP_CONTENT,
                RelativeLayout.LayoutParams.WRAP_CONTENT);
        kvLp.addRule(RelativeLayout.CENTER_HORIZONTAL);
        kvLp.addRule(RelativeLayout.ALIGN_PARENT_BOTTOM);
        keyboardView.setLayoutParams(kvLp);
        root.addView(keyboardView);

        setContentView(root);

        touchpad = new VirtualTouchpad(this);
        inputMode = getSharedPreferences(PREFS, MODE_PRIVATE)
                .getInt(PREF_INPUT, INPUT_TOUCH);
        nativeHandle = Native.nativeCreate();

        /* 真·事件驱动：内核 commit / pipe_enable 时 signal App eventfd，
         * native 渲染线程回调这里。回调在非 UI 线程，转回主线程再转屏。*/
        Native.setModeListener(() -> runOnUiThread(() -> {
            if (!isFinishing() && !isDestroyed())
                syncOrientation();
            /* 换档的同时把通知里的状态也刷一遍：这里天然是"参数刚变过"的
             * 时刻，不需要另开轮询。*/
            ForegroundService.requestRefresh(this);
        }));

        syncOrientation();
        orientHandler.postDelayed(orientPoll, ORIENT_POLL_MS);
    }

    /** 读一次 KPM 的生效几何，方向变了就申请转屏。 */
    private void syncOrientation() {
        int packed = Native.nativeGetMode();
        if (packed <= 0) return;
        int w = (packed >>> 16) & 0xffff;
        int h = packed & 0xffff;
        if (w <= 0 || h <= 0) return;

        int want = (h > w) ? ActivityInfo.SCREEN_ORIENTATION_PORTRAIT
                           : ActivityInfo.SCREEN_ORIENTATION_LANDSCAPE;

        /* appliedOrientation 只记“申请过”，不代表窗口真的转过去了。
         * 启动时 Activity 可能先是手机默认的竖屏；如果只比 appliedOrientation，
         * 就会以为已经申请过而不再 setRequestedOrientation，窗口永远停在竖屏，
         * 横屏帧被 letterbox 缩进去。这里再对一次实际 Configuration。*/
        int current = getResources().getConfiguration().orientation;
        boolean windowMatches =
            (want == ActivityInfo.SCREEN_ORIENTATION_PORTRAIT &&
             current == Configuration.ORIENTATION_PORTRAIT) ||
            (want == ActivityInfo.SCREEN_ORIENTATION_LANDSCAPE &&
             current == Configuration.ORIENTATION_LANDSCAPE);
        if (want == appliedOrientation && windowMatches) return;
        appliedOrientation = want;
        Log.i(TAG, "orientation -> "
                + (want == ActivityInfo.SCREEN_ORIENTATION_PORTRAIT
                   ? "portrait" : "landscape")
                + " (KPM " + w + "x" + h + ")");
        /* configChanges 里有 orientation|screenSize，所以不会重建 Activity，
         * SurfaceView 的尺寸变化走 surfaceChanged。*/
        setRequestedOrientation(want);
    }

    /* 从后台回到前台：通知 native 一声"就绪"。
     *
     * 后台时 SF 不再合成这个图层 → present 不来 → 内核的帧事件只能等存活超时，
     * 桌面掉到个位数帧率。干等合成器下一次提交的话，切回来那一下会先停一下
     * 再醒 —— 就是肉眼看到的卡顿。nativeSetReady() 让内核立刻把当前帧重推
     * 一次，并把最后一帧重贴回图层，恢复是瞬时的。*/
    @Override
    protected void onPause() {
        if (nativeHandle != 0)
            Native.nativeSetPaused(nativeHandle, true);
        super.onPause();
    }

    @Override
    protected void onResume() {
        super.onResume();
        if (nativeHandle != 0) {
            Native.nativeSetPaused(nativeHandle, false);
            Native.nativeSetReady(nativeHandle);
        }
        /* 回前台顺手把通知状态对一次（用户这时候才会看它） */
        ForegroundService.requestRefresh(this);
    }

    @Override
    protected void onDestroy() {
        Native.setModeListener(null);
        orientHandler.removeCallbacks(orientPoll);
        Native.nativeDestroy(nativeHandle);
        nativeHandle = 0;
        stopForegroundService();
        super.onDestroy();
    }

    /* ---- Surface callbacks ---- */

    @Override
    public void surfaceCreated(SurfaceHolder holder) {}

    @Override
    public void surfaceChanged(SurfaceHolder holder, int format, int width, int height) {
        keyboardView.setScreenSize(width, height);
        /* 同上：不再对 Surface 投 120Hz。 */
        Log.i(TAG, "VDRM build window-no-fps-vote");
        Native.nativeStart(nativeHandle, holder.getSurface());

        /* fd 已经就绪：把调试页里存的 pace 真正下发一次。*/
        VdrmSettingsActivity.applySaved(this);
        Native.nativeStartAudio(nativeHandle);
    }

    @Override
    public void surfaceDestroyed(SurfaceHolder holder) {
        Native.nativeStop(nativeHandle);
    }

    /** 直接触摸：把当前活动触点打包发给内核 vdrm-touch（ABS_MT）。
     *  每帧一条 ioctl；内核负责自动 lift、BTN_TOUCH 和唯一 SYN。*/
    private boolean sendTouchFrame(MotionEvent event) {
        int action = event.getActionMasked();
        int skip = (action == MotionEvent.ACTION_POINTER_UP ||
                    action == MotionEvent.ACTION_UP)
                   ? event.getActionIndex() : -1;
        int n = 0;

        if (action != MotionEvent.ACTION_UP &&
            action != MotionEvent.ACTION_CANCEL) {
            for (int i = 0; i < event.getPointerCount(); i++)
                if (i != skip) n++;
        }
        if (n > TOUCH_MAX) n = TOUCH_MAX;

        int k = 0;
        for (int i = 0; i < event.getPointerCount() && k < n; i++) {
            if (i == skip) continue;
            touchIds[k] = event.getPointerId(i);
            touchXs[k] = event.getX(i);
            touchYs[k] = event.getY(i);
            touchPs[k] = event.getPressure(i);
            k++;
        }
        Native.nativeSendTouch(n, touchIds, touchXs, touchYs, touchPs);
        return true;
    }

    /** 横竖切换 / 小窗↔全屏 等配置变化。
     *
     *  以前这里会把键盘收起来，因为键盘大小是按窗口比例算的、重排会跳一下。
     *  现在键盘大小定死了（见 VirtualKeyboardView.setScreenSize），转屏之后
     *  尺寸不变，只是重新贴到底部，所以让它一直显示着，不用再藏。*/
    @Override
    public void onConfigurationChanged(Configuration newConfig) {
        super.onConfigurationChanged(newConfig);
        Log.i(TAG, "config changed -> orientation "
                + newConfig.orientation + ", keyboard kept");
    }

    /* ---- Key interception ---- */

    /* 音量键不拦。
     *
     * 以前这里把 VOLUME_UP/DOWN 转发进容器（115/114）然后 return true ——
     * 只要这块窗口是焦点（也就是容器正在出画面的时候），手机的音量键就被
     * 整个吞掉：音量的加减完全没反应，看起来像"音量炸了"。
     * 而且容器里的虚拟键盘现在也不再声明 113/114/115（见内核 vdrm_key_system），
     * 转发过去也是空转。
     *
     * 更关键的是：容器里的声音是 App 用 AudioTrack 播到手机上的，
     * 所以手机的音量键本来就该调手机的媒体音量 —— 交给系统处理才对。*/
    @Override
    public boolean onKeyDown(int keyCode, KeyEvent event) {
        if (keyCode == KeyEvent.KEYCODE_BACK) {
            long now = System.currentTimeMillis();
            if (now - lastBackTime < 500) {
                finish();
                return true;
            }
            lastBackTime = now;
            keyboardView.setVisibility(
                keyboardView.getVisibility() == View.VISIBLE ? View.GONE : View.VISIBLE);
            return true;
        }
        int lc = linuxKeyCode(keyCode);
        if (lc >= 0) {
            Native.nativeSendKey(lc, true);
            return true;
        }
        return super.onKeyDown(keyCode, event);
    }

    @Override
    public boolean onKeyUp(int keyCode, KeyEvent event) {
        if (keyCode == KeyEvent.KEYCODE_BACK) {
            return true;
        }
        int lc = linuxKeyCode(keyCode);
        if (lc >= 0) {
            Native.nativeSendKey(lc, false);
            return true;
        }
        return super.onKeyUp(keyCode, event);
    }

    /* Map Android keycode → Linux input keycode */
    private static int linuxKeyCode(int androidCode) {
        switch (androidCode) {
            case KeyEvent.KEYCODE_A: return 30;
            case KeyEvent.KEYCODE_B: return 48;
            case KeyEvent.KEYCODE_C: return 46;
            case KeyEvent.KEYCODE_D: return 32;
            case KeyEvent.KEYCODE_E: return 18;
            case KeyEvent.KEYCODE_F: return 33;
            case KeyEvent.KEYCODE_G: return 34;
            case KeyEvent.KEYCODE_H: return 35;
            case KeyEvent.KEYCODE_I: return 23;
            case KeyEvent.KEYCODE_J: return 36;
            case KeyEvent.KEYCODE_K: return 37;
            case KeyEvent.KEYCODE_L: return 38;
            case KeyEvent.KEYCODE_M: return 50;
            case KeyEvent.KEYCODE_N: return 49;
            case KeyEvent.KEYCODE_O: return 24;
            case KeyEvent.KEYCODE_P: return 25;
            case KeyEvent.KEYCODE_Q: return 16;
            case KeyEvent.KEYCODE_R: return 19;
            case KeyEvent.KEYCODE_S: return 31;
            case KeyEvent.KEYCODE_T: return 20;
            case KeyEvent.KEYCODE_U: return 22;
            case KeyEvent.KEYCODE_V: return 47;
            case KeyEvent.KEYCODE_W: return 17;
            case KeyEvent.KEYCODE_X: return 45;
            case KeyEvent.KEYCODE_Y: return 21;
            case KeyEvent.KEYCODE_Z: return 44;
            case KeyEvent.KEYCODE_0: return 11;
            case KeyEvent.KEYCODE_1: return 2;
            case KeyEvent.KEYCODE_2: return 3;
            case KeyEvent.KEYCODE_3: return 4;
            case KeyEvent.KEYCODE_4: return 5;
            case KeyEvent.KEYCODE_5: return 6;
            case KeyEvent.KEYCODE_6: return 7;
            case KeyEvent.KEYCODE_7: return 8;
            case KeyEvent.KEYCODE_8: return 9;
            case KeyEvent.KEYCODE_9: return 10;
            case KeyEvent.KEYCODE_SPACE: return 57;
            case KeyEvent.KEYCODE_ENTER: return 28;
            case KeyEvent.KEYCODE_TAB: return 15;
            case KeyEvent.KEYCODE_SHIFT_LEFT: return 42;
            case KeyEvent.KEYCODE_SHIFT_RIGHT: return 54;
            case KeyEvent.KEYCODE_CTRL_LEFT: return 29;
            case KeyEvent.KEYCODE_CTRL_RIGHT: return 97;
            case KeyEvent.KEYCODE_ALT_LEFT: return 56;
            case KeyEvent.KEYCODE_ALT_RIGHT: return 100;
            case KeyEvent.KEYCODE_META_LEFT: return 125;
            case KeyEvent.KEYCODE_META_RIGHT: return 126;
            case KeyEvent.KEYCODE_DEL: return 14;
            case KeyEvent.KEYCODE_FORWARD_DEL: return 111;
            case KeyEvent.KEYCODE_ESCAPE: return 1;
            case KeyEvent.KEYCODE_DPAD_UP: return 103;
            case KeyEvent.KEYCODE_DPAD_DOWN: return 108;
            case KeyEvent.KEYCODE_DPAD_LEFT: return 105;
            case KeyEvent.KEYCODE_DPAD_RIGHT: return 106;
            case KeyEvent.KEYCODE_PAGE_UP: return 104;
            case KeyEvent.KEYCODE_PAGE_DOWN: return 109;
            case KeyEvent.KEYCODE_MOVE_END: return 107;
            case KeyEvent.KEYCODE_INSERT: return 110;
            case KeyEvent.KEYCODE_F1: return 59;
            case KeyEvent.KEYCODE_F2: return 60;
            case KeyEvent.KEYCODE_F3: return 61;
            case KeyEvent.KEYCODE_F4: return 62;
            case KeyEvent.KEYCODE_F5: return 63;
            case KeyEvent.KEYCODE_F6: return 64;
            case KeyEvent.KEYCODE_F7: return 65;
            case KeyEvent.KEYCODE_F8: return 66;
            case KeyEvent.KEYCODE_F9: return 67;
            case KeyEvent.KEYCODE_F10: return 68;
            case KeyEvent.KEYCODE_F11: return 87;
            case KeyEvent.KEYCODE_F12: return 88;
            case KeyEvent.KEYCODE_COMMA: return 51;
            case KeyEvent.KEYCODE_PERIOD: return 52;
            case KeyEvent.KEYCODE_SLASH: return 53;
            case KeyEvent.KEYCODE_SEMICOLON: return 39;
            case KeyEvent.KEYCODE_APOSTROPHE: return 40;
            case KeyEvent.KEYCODE_MINUS: return 12;
            case KeyEvent.KEYCODE_EQUALS: return 13;
            case KeyEvent.KEYCODE_GRAVE: return 41;
            case KeyEvent.KEYCODE_LEFT_BRACKET: return 26;
            case KeyEvent.KEYCODE_RIGHT_BRACKET: return 27;
            case KeyEvent.KEYCODE_BACKSLASH: return 43;
            case KeyEvent.KEYCODE_NUMPAD_ENTER: return 96;
            default: return -1;
        }
    }

    /* ---- Permissions ---- */

    private void requestPermissions() {
        boolean needNotify = false, needAudio = false;
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
            if (checkSelfPermission(android.Manifest.permission.POST_NOTIFICATIONS)
                    != PackageManager.PERMISSION_GRANTED) {
                needNotify = true;
            }
        }
        if (checkSelfPermission(android.Manifest.permission.RECORD_AUDIO)
                != PackageManager.PERMISSION_GRANTED) {
            needAudio = true;
        }
        if (needNotify || needAudio) {
            String[] perms = new String[(needNotify ? 1 : 0) + (needAudio ? 1 : 0)];
            int i = 0;
            if (needNotify) perms[i++] = android.Manifest.permission.POST_NOTIFICATIONS;
            if (needAudio)  perms[i++] = android.Manifest.permission.RECORD_AUDIO;
            requestPermissions(perms, 1001);
        }
    }

    @Override
    public void onRequestPermissionsResult(int requestCode, String[] permissions,
                                           int[] grantResults) {
        super.onRequestPermissionsResult(requestCode, permissions, grantResults);
        if (requestCode == 1001) {
            for (int i = 0; i < grantResults.length; i++) {
                if (permissions[i].equals(android.Manifest.permission.POST_NOTIFICATIONS)
                        && grantResults[i] == PackageManager.PERMISSION_GRANTED) {
                    startForegroundService();
                }
            }
        }
    }

    /* ---- Foreground Service ---- */

    private void startForegroundService() {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
            if (checkSelfPermission(android.Manifest.permission.POST_NOTIFICATIONS)
                    != PackageManager.PERMISSION_GRANTED) {
                return;  /* notification not granted, skip foreground service */
            }
        }
        Intent intent = new Intent(this, ForegroundService.class);
        try {
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
                startForegroundService(intent);
            } else {
                startService(intent);
            }
        } catch (Exception e) {
            /* foreground service start failed — continue without it */
        }
    }

    private void stopForegroundService() {
        Intent intent = new Intent(this, ForegroundService.class);
        stopService(intent);
    }
}