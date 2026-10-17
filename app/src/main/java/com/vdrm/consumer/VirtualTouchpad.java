package com.vdrm.consumer;

import android.content.Context;
import android.view.MotionEvent;
import android.view.ViewConfiguration;

/**
 * 相对触控板（位移处理参考 anland 的 Touchpad，去掉手势转发等非核心部分）。
 *
 *   1 指拖动       → 光标相对移动：平滑(死区 + IIR + 累积) × 加速曲线
 *   1 指轻点       → 左键单击
 *   1 指长按 500ms → 拖拽（左键按住，跟随手指）
 *   2 指拖动       → 滚动
 *   2 指轻点       → 右键单击
 *
 * 加速曲线（同 anland）：
 *   scale = 1 + (ACCEL-1) * (speed/(1+speed))，clamp[0.3, 10]
 *   ACCEL = 1.0 → 不提速（位移 1:1）；ACCEL > 1 → "非 1:1"（手指移 1px 光标移更多）。
 *   ACCEL 就是唯一的灵敏度旋钮，以后可接设置滑块。
 */
public final class VirtualTouchpad {

    private static final int STATE_IDLE = 0;
    private static final int STATE_ONE  = 1;
    private static final int STATE_TWO  = 2;
    private static final int STATE_DRAG = 3;

    private static final long  LONG_PRESS_MS = 500;
    private static final long  TAP_MS        = 300;
    private static final int   BTN_LEFT  = 0x110;   /* 272 */
    private static final int   BTN_RIGHT = 0x111;   /* 273 */

    /* ---- 位移处理（与 anland 数值一致）---- */
    private static final float DEAD_ZONE        = 0.3f;
    private static final float SMOOTHING_FACTOR = 0.45f;
    private static final float ACCUM_THRESHOLD  = 0.1f;
    private static final float ACCEL            = 2.0f;   /* 灵敏度：>1 即非 1:1 */
    private static final float SCROLL_SPEED     = 0.5f;  /* 双指位移 → 滚动量 */

    private int state = STATE_IDLE;
    private float startX, startY, lastX, lastY, lastX2, lastY2;
    private long downTime;
    private boolean tapCandidate, twoTapCandidate, dragging, multi;
    private final float touchSlop;

    /* 平滑/累积状态 */
    private float smoothedDx, smoothedDy;
    private float accumX, accumY;       /* 平滑后的累积（阈值 0.1 才放行）*/
    private float moveAccX, moveAccY;   /* 加速后再累积，避免 (int) 丢精度 */
    private float scrollAccX, scrollAccY;   /* 滚动的亚像素累积（截断前累加）*/
    private boolean smoothInit;

    VirtualTouchpad(Context context) {
        touchSlop = ViewConfiguration.get(context).getScaledTouchSlop();
    }

    boolean onTouch(MotionEvent event) {
        switch (event.getActionMasked()) {
        case MotionEvent.ACTION_DOWN:
            startX = lastX = event.getX();
            startY = lastY = event.getY();
            downTime = event.getEventTime();
            tapCandidate = true;
            twoTapCandidate = false;
            dragging = false;
            multi = false;
            state = STATE_ONE;
            resetSmoothing();
            break;

        case MotionEvent.ACTION_POINTER_DOWN:
            multi = true;
            tapCandidate = false;
            if (dragging) {
                Native.nativeSendBtn(BTN_LEFT, false);
                dragging = false;
            }
            if (event.getPointerCount() == 2) {
                state = STATE_TWO;
                twoTapCandidate = true;
                lastX  = event.getX(0); lastY  = event.getY(0);
                lastX2 = event.getX(1); lastY2 = event.getY(1);
            }
            break;

        case MotionEvent.ACTION_MOVE:
            if (event.getPointerCount() == 1 && !multi) {
                float x = event.getX();
                float y = event.getY();
                float rawDx = x - lastX;
                float rawDy = y - lastY;
                if (Math.hypot(x - startX, y - startY) > touchSlop)
                    tapCandidate = false;

                if (tapCandidate && !dragging
                        && event.getEventTime() - downTime >= LONG_PRESS_MS) {
                    dragging = true;
                    state = STATE_DRAG;
                    Native.nativeSendBtn(BTN_LEFT, true);
                    resetSmoothing();
                } else {
                    float[] s = applySmoothing(rawDx, rawDy);
                    if (s[0] != 0f || s[1] != 0f) {
                        float distance = (float) Math.hypot(s[0], s[1]);
                        float speed = distance / 10.0f;
                        float scale = 1.0f + (ACCEL - 1.0f) * (speed / (1.0f + speed));
                        if (scale < 0.3f) scale = 0.3f;
                        else if (scale > 10.0f) scale = 10.0f;
                        moveAccX += s[0] * scale;
                        moveAccY += s[1] * scale;
                        int mx = (int) moveAccX;
                        int my = (int) moveAccY;
                        if (mx != 0 || my != 0) {
                            moveAccX -= mx;
                            moveAccY -= my;
                            Native.nativeSendMotion(mx, my);
                        }
                    }
                }
                lastX = x;
                lastY = y;
            } else if (event.getPointerCount() == 2 && state == STATE_TWO) {
                float x0 = event.getX(0), y0 = event.getY(0);
                float x1 = event.getX(1), y1 = event.getY(1);
                float adx = ((x0 - lastX) + (x1 - lastX2)) / 2f;
                float ady = ((y0 - lastY) + (y1 - lastY2)) / 2f;
                if (Math.abs(adx) > 1f || Math.abs(ady) > 1f) {
                    twoTapCandidate = false;
                    /* 自然方向：手指上滑 → 内容下滑。
                     * ★必须先累加再截断：每帧位移 × 0.5 常常不足 1（慢滑时
                     * 1~2px 的步长整段落在死带），直接 (int) 就每个事件都归零。
                     * 累积器保留小数部分，过 ±1 才发，慢滑才不丢。*/
                    if (Math.abs(ady) > Math.abs(adx) * 0.5f) {
                        scrollAccY -= ady * SCROLL_SPEED;
                        int sy = (int) scrollAccY;
                        if (sy != 0) {
                            scrollAccY -= sy;
                            Native.nativeSendScroll(0, sy);
                        }
                    }
                    if (Math.abs(adx) > Math.abs(ady) * 0.5f) {
                        scrollAccX += adx * SCROLL_SPEED;
                        int sx = (int) scrollAccX;
                        if (sx != 0) {
                            scrollAccX -= sx;
                            Native.nativeSendScroll(1, sx);
                        }
                    }
                    lastX = x0; lastY = y0;
                    lastX2 = x1; lastY2 = y1;
                }
            }
            break;

        case MotionEvent.ACTION_POINTER_UP:
            if (event.getPointerCount() - 1 == 1) {
                /* 回到单指：以剩下那根手指为基线 */
                multi = false;
                tapCandidate = false;
                int idx = (event.getActionIndex() == 0) ? 1 : 0;
                startX = lastX = event.getX(idx);
                startY = lastY = event.getY(idx);
                downTime = event.getEventTime();
                state = STATE_ONE;
                resetSmoothing();
            }
            break;

        case MotionEvent.ACTION_UP:
            long dur = event.getEventTime() - downTime;
            if (dragging) {
                Native.nativeSendBtn(BTN_LEFT, false);
                dragging = false;
            } else if (twoTapCandidate && dur < TAP_MS) {
                Native.nativeSendBtn(BTN_RIGHT, true);
                Native.nativeSendBtn(BTN_RIGHT, false);
            } else if (state == STATE_ONE && tapCandidate && dur < TAP_MS) {
                Native.nativeSendBtn(BTN_LEFT, true);
                Native.nativeSendBtn(BTN_LEFT, false);
            }
            reset();
            break;

        case MotionEvent.ACTION_CANCEL:
            if (dragging) {
                Native.nativeSendBtn(BTN_LEFT, false);
                dragging = false;
            }
            reset();
            break;
        }
        return true;
    }

    private void reset() {
        state = STATE_IDLE;
        tapCandidate = false;
        twoTapCandidate = false;
        dragging = false;
        multi = false;
        resetSmoothing();
    }

    private void resetSmoothing() {
        smoothedDx = smoothedDy = 0f;
        accumX = accumY = 0f;
        moveAccX = moveAccY = 0f;
        scrollAccX = scrollAccY = 0f;
        smoothInit = false;
    }

    /** 死区 → IIR 平滑 → 阈值累积（与 anland 一致）。返回本次放行的位移。 */
    private float[] applySmoothing(float rawDx, float rawDy) {
        float deadDx = Math.abs(rawDx) < DEAD_ZONE ? 0f : rawDx;
        float deadDy = Math.abs(rawDy) < DEAD_ZONE ? 0f : rawDy;

        if (deadDx == 0f && deadDy == 0f)
            return new float[]{0f, 0f};

        if (!smoothInit) {
            smoothedDx = deadDx;
            smoothedDy = deadDy;
            smoothInit = true;
        } else {
            smoothedDx = SMOOTHING_FACTOR * deadDx + (1 - SMOOTHING_FACTOR) * smoothedDx;
            smoothedDy = SMOOTHING_FACTOR * deadDy + (1 - SMOOTHING_FACTOR) * smoothedDy;
        }

        accumX += smoothedDx;
        accumY += smoothedDy;

        float outX = 0f, outY = 0f;
        if (Math.abs(accumX) >= ACCUM_THRESHOLD) {
            outX = accumX;
            accumX = 0f;
        }
        if (Math.abs(accumY) >= ACCUM_THRESHOLD) {
            outY = accumY;
            accumY = 0f;
        }
        return new float[]{outX, outY};
    }
}
