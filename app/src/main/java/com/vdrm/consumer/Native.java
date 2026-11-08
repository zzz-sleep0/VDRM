package com.vdrm.consumer;

import android.view.Surface;

public class Native {
    static { System.loadLibrary("vdrm_consumer"); }

    public static native long nativeCreate();
    public static native void nativeDestroy(long handle);
    public static native void nativeStart(long handle, Surface surface);
    public static native void nativeStop(long handle);

    /** 回到前台时调：让内核把当前帧立刻重推一次 + 把最后一帧重贴回图层。
     *  老模块没有这个 ioctl，native 侧会吞掉 ENOTTY。 */
    public static native void nativeSetReady(long handle);

    /** App 进后台/回前台。后台时 native 侧停止 latch，帧立即 RETURN，
     *  present/held/拷贝环全部放掉；回前台再 nativeSetReady() 重推。 */
    public static native void nativeSetPaused(long handle, boolean paused);

    /* Event senders (static, no handle needed) */
    public static native int nativeSendKey(int code, boolean down);
    public static native int nativeSendMotion(int dx, int dy);
    public static native int nativeSendBtn(int btn, boolean pressed);
    public static native int nativeSendScroll(int axis, int val);

    /* 当前 KPM 输出几何：(w << 16) | h；拿不到返回 0。
     * Activity 靠它决定自己该横着还是竖着。
     * commit 之后、合成器 pipe_enable 之前会先返回目标尺寸（见 native 侧），
     * 让窗口提前转。 */
    public static native int nativeGetMode();

    /** 内核 mode 事件（commit / g_active 变化）的回调。渲染线程调用，非 UI 线程。 */
    public interface ModeListener { void onModeChanged(); }
    private static volatile ModeListener sModeListener;

    public static void setModeListener(ModeListener l) { sModeListener = l; }

    /* 由 native 的 notify_mode_changed() 调用（JNI: ()V）。 */
    public static void onModeChanged() {
        ModeListener l = sModeListener;
        if (l != null) l.onModeChanged();
    }

    /* Direct touch (ABS_MT): active contacts packed per frame.
     * count == 0 means "all lifted". */
    public static native int nativeSendTouch(int count, int[] ids,
                                             float[] xs, float[] ys,
                                             float[] ps);

    /* ---- 调试页（长按图标 → 调试选项）----
     * inflight: 1..6，还没上屏的帧数上限
     * pll: 假 vblank 是否锁到面板 vsync 上 */
    public static native void nativeSetInflight(int n);
    public static native void nativeSetPll(boolean on);

    /* Audio (reserved) */
    public static native void nativeStartAudio(long handle);
    public static native void nativeStopAudio(long handle);
}