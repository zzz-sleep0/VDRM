package com.vdrm.consumer;

import android.content.Context;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.Paint;
import android.graphics.RectF;
import android.view.MotionEvent;
import android.view.View;
import android.view.ViewConfiguration;

public class VirtualKeyboardView extends View {

    private Paint bgPaint, keyPaint, textPaint, pressPaint, barPaint;
    private int pressedKey = -1;
    private boolean fnMode = false;
    private boolean shiftMode = false;
    private boolean shiftLock = false;   /* 双击 Shift 锁定 */
    private boolean capsMode = false;    /* Caps 状态（仅用于显示；实际 lock 由合成器处理）*/
    /* 粘滞修饰键：软键盘单指按不住组合键，改成 tap 锁定，一次性作用于下一个键 */
    private boolean ctrlLatch = false, altLatch = false, superLatch = false;
    private int     pressedMods = 0;     /* 当前键已发出的修饰键位：1=Shift 2=Ctrl 4=Alt 8=Super */
    private long    lastShiftTap = 0;

    private static final int BAR_W = 24;
    private static final int CTRL_GAP = 20;

    /* 定死的大小（见 setScreenSize）：算一次，之后不再变。*/
    private int fixedW, fixedH;
    private int kbW, kbH;
    private int screenW, screenH;

    private int dragBarY, dragBarLeft, dragBarRight;
    private int keyAreaTop;

    private boolean isDragging = false;
    private float dragDownX, dragDownY;
    private float dragAccumX, dragAccumY;
    private int dragSlop;
    private int dragPointerId = -1;

    private int[] rowKeyCount;
    private int[] rowWideCount;
    private int[] rowKeyWidth;
    private int[] rowWideWidth;
    private int keyH;
    private int keyAreaH;

    private static final String[][] KEYS = {
        {"`","41","~","41",  "1","2","!","2",  "2","3","@","3",
         "3","4","#","4",  "4","5","$","5",  "5","6","%","6",
         "6","7","^","7",  "7","8","&","8",  "8","9","*","9",
         "9","10","(","10",  "0","11",")","11",
         "-","12","_","12",  "=","13","+","13",
         "Backspace","14",null,null},
        {"Tab","15",null,null,  "q","16",null,null,
         "w","17",null,null,  "e","18",null,null,
         "r","19",null,null,  "t","20",null,null,
         "y","21",null,null,  "u","22",null,null,
         "i","23",null,null,  "o","24",null,null,
         "p","25",null,null,  "[","26","{","26",
         "]","27","}","27",  "\\","43","|","43"},
        {"Caps","58",null,null,  "a","30",null,null,
         "s","31",null,null,  "d","32",null,null,
         "f","33",null,null,  "g","34",null,null,
         "h","35",null,null,  "j","36",null,null,
         "k","37",null,null,  "l","38",null,null,
         ";","39",":","39",  "'","40","\"","40",
         "Enter","28",null,null},
        {"Shift","-2",null,null,  "z","44",null,null,
         "x","45",null,null,  "c","46",null,null,
         "v","47",null,null,  "b","48",null,null,
         "n","49",null,null,  "m","50",null,null,
         ",","51","<","51",  ".","52",">","52",
         "/","53","?","53",  "Shift","-2",null,null},
        {"Ctrl","29",null,null,  "Super","125",null,null,
         "Alt","56",null,null,
         "     Space     ","57",null,null,
         "Alt","56",null,null,  "Fn","-1",null,null},
        {"Esc","1",null,null,  "←","105",null,null,  "↑","103",null,null,
         "↓","108",null,null,  "→","106",null,null,  "Home","102",null,null,
         "End","107",null,null,  "PgUp","104",null,null,  "PgDn","109",null,null,
         "Del","111",null,null,  "Ins","110",null,null},
    };

    private static final int KEY_GAP = 3;

    private static final String[] FN_ROW0 = {
        null, null,
        "F1","59",  "F2","60",  "F3","61",  "F4","62",  "F5","63",
        "F6","64",  "F7","65",  "F8","66",  "F9","67",  "F10","68",
        null, null,  null, null,  null, null,  null, null,
    };

    private RectF[][] keyRects;
    private int[][] keyInnerCols;

    public VirtualKeyboardView(Context context) {
        super(context);
        bgPaint = new Paint();
        bgPaint.setColor(0x80000000);
        bgPaint.setStyle(Paint.Style.FILL);

        keyPaint = new Paint();
        keyPaint.setColor(0xA0A0A0A0);
        keyPaint.setStyle(Paint.Style.FILL);

        pressPaint = new Paint();
        pressPaint.setColor(0xFFCCCCCC);

        textPaint = new Paint();
        textPaint.setColor(Color.WHITE);
        textPaint.setTextAlign(Paint.Align.CENTER);
        textPaint.setAntiAlias(true);

        barPaint = new Paint();
        barPaint.setColor(0xFF505050);
        barPaint.setStyle(Paint.Style.FILL);


        dragSlop = ViewConfiguration.get(context).getScaledTouchSlop();

        initRowData();
    }

    private void initRowData() {
        int rows = KEYS.length;
        rowKeyCount = new int[rows];
        rowWideCount = new int[rows];
        for (int r = 0; r < rows; r++) {
            int n = 0, w = 0;
            for (int i = 0; i < KEYS[r].length; i += 4) {
                String label = KEYS[r][i];
                if (label == null) continue;
                n++;
                if (label.length() > 3) w++;
            }
            rowKeyCount[r] = n;
            rowWideCount[r] = w;
        }
    }

    /* 键盘大小**定死**，不跟屏幕横竖走。
     *
     * 以前是 baseW = 屏宽*4/5、baseH = 屏高/2 —— 于是屏幕一转竖，宽度被压到
     * 竖屏宽的一半（横向过窄）、高度却按竖屏高算（纵向过长），键位全变形。
     *
     * 现在只按屏幕**短边**定一次：
     *   宽 = 短边 - 两侧留白，高 = 短边 / 2
     * 横屏（2780x1264）和竖屏（1264x2780）算出来完全一样（1224 x 632），
     * 而且这个宽度一定塞得进竖屏。窗口比它还窄（分屏/小窗）时再夹一下。
     *
     * 大小、透明度都不再可调；唯一能调的是位置（拖上面的横条）。*/
    public void setScreenSize(int w, int h) {
        screenW = w;
        screenH = h;

        int shortSide = Math.min(w, h);
        fixedW = shortSide - 2 * CTRL_GAP;
        fixedH = shortSide / 2;

        kbW = Math.min(fixedW, Math.max(200, w));
        kbH = fixedH;
    }

    private void recomputeLayout() {
        int gap = KEY_GAP;
        int rows = KEYS.length;

        keyH = (kbH - 10 - (rows - 1) * gap) / rows;
        if (keyH < 40) keyH = 40;

        rowKeyWidth = new int[rows];
        rowWideWidth = new int[rows];
        for (int r = 0; r < rows; r++) {
            int n = rowKeyCount[r] - rowWideCount[r];
            int wc = rowWideCount[r];
            int avail = kbW - (rowKeyCount[r] - 1) * gap;
            int totalU = n + wc * 2;
            int unit = avail / totalU;
            rowKeyWidth[r] = unit;
            rowWideWidth[r] = unit * 2 + gap;
        }

        keyAreaH = rows * keyH + (rows - 1) * gap;

        dragBarY = CTRL_GAP;
        keyAreaTop = dragBarY + BAR_W + CTRL_GAP;

        int dragBarWidth = BAR_W * 5;
        dragBarLeft = (kbW - dragBarWidth) / 2;
        dragBarRight = dragBarLeft + dragBarWidth;

        recomputeKeyRects();
    }

    private void recomputeKeyRects() {
        int gap = KEY_GAP;
        int rows = KEYS.length;
        keyRects = new RectF[rows][];
        keyInnerCols = new int[rows][];

        for (int r = 0; r < rows; r++) {
            int kw = rowKeyWidth[r];
            int ww = rowWideWidth[r];
            int y = keyAreaTop + r * (keyH + gap);

            int valid = 0;
            for (int i = 0; i < KEYS[r].length; i += 4) {
                if (KEYS[r][i] != null) valid++;
            }

            keyRects[r] = new RectF[valid];
            keyInnerCols[r] = new int[valid];

            int rowW = 0;
            for (int i = 0; i < KEYS[r].length; i += 4) {
                String label = KEYS[r][i];
                if (label == null) continue;
                rowW += (label.length() > 3 ? ww : kw) + gap;
            }
            rowW -= gap;

            int startX = (kbW - rowW) / 2;
            int cx = startX;
            int idx = 0;
            for (int i = 0; i < KEYS[r].length; i += 4) {
                String label = KEYS[r][i];
                if (label == null) continue;
                int kx = label.length() > 3 ? ww : kw;
                keyRects[r][idx] = new RectF(cx, y, cx + kx, y + keyH);
                keyInnerCols[r][idx] = i / 4;
                cx += kx + gap;
                idx++;
            }
        }
    }

    @Override
    protected void onMeasure(int widthMeasureSpec, int heightMeasureSpec) {
        int parentW = MeasureSpec.getSize(widthMeasureSpec);
        int parentH = MeasureSpec.getSize(heightMeasureSpec);
        if (screenW == 0) setScreenSize(parentW, parentH);

        recomputeLayout();

        /* 视图就是键盘本身，右边不再挂透明度条、右上角不再挂缩放点。*/
        setMeasuredDimension(kbW, keyAreaTop + keyAreaH + CTRL_GAP);
    }

    @Override
    protected void onDraw(Canvas canvas) {
        super.onDraw(canvas);

        canvas.drawRect(0, keyAreaTop, kbW, keyAreaTop + keyAreaH, bgPaint);

        RectF dragBar = new RectF(dragBarLeft, dragBarY, dragBarRight, dragBarY + BAR_W);
        canvas.drawRoundRect(dragBar, BAR_W / 2f, BAR_W / 2f, barPaint);

        for (int r = 0; r < KEYS.length; r++) {
            int kw = rowKeyWidth[r];

            for (int i = 0; i < KEYS[r].length; i += 4) {
                String label = KEYS[r][i];
                if (label == null) continue;
                String shiftLabel = KEYS[r][i + 2];
                int c = i / 4;

                boolean isWide = label.length() > 3;
                boolean isFnKey = fnMode && r == 0 && c * 2 < FN_ROW0.length && FN_ROW0[c * 2] != null;
                int idx = r * 20 + c;

                RectF rect = keyRects[r][i / 4];
                if (rect == null) continue;

                boolean lit = idx == pressedKey
                        || (label.equals("Shift") && shiftMode)
                        || (label.equals("Caps") && capsMode)
                        || (label.equals("Ctrl") && ctrlLatch)
                        || (label.equals("Alt") && altLatch)
                        || (label.equals("Super") && superLatch);
                canvas.drawRoundRect(rect, 5, 5, lit ? pressPaint : keyPaint);

                String display = label;
                if (isFnKey) {
                    display = FN_ROW0[c * 2];
                } else if (isLetter(label)) {
                    display = (shiftMode != capsMode) ? label.toUpperCase() : label.toLowerCase();
                } else if (shiftMode && shiftLabel != null) {
                    display = shiftLabel;
                }

                float mainSize = isWide ? 36 : Math.min(48, kw * 0.58f);
                textPaint.setTextSize(mainSize);
                canvas.drawText(display, rect.centerX(), rect.centerY() + mainSize * 0.35f, textPaint);

                if (r == 0 && !fnMode && c * 2 < FN_ROW0.length && FN_ROW0[c * 2] != null) {
                    float fnSize = Math.min(22, kw * 0.28f);
                    textPaint.setTextSize(fnSize);
                    canvas.drawText(FN_ROW0[c * 2], rect.centerX(), rect.top + fnSize + 3, textPaint);
                }
            }
        }
    }

    private int hitTest(float x, float y) {
        for (int r = 0; r < keyRects.length; r++) {
            for (int c = 0; c < keyRects[r].length; c++) {
                if (keyRects[r][c] != null && keyRects[r][c].contains(x, y)) {
                    int col = keyInnerCols[r][c];
                    return r * 20 + col;
                }
            }
        }
        return -1;
    }

    private static boolean isModifierCode(int code) {
        return code == 29 || code == 97 || code == 56 || code == 100
                || code == 125 || code == 126 || code == 58;
    }

    private static boolean isLetter(String s) {
        return s.length() == 1 && Character.isLetter(s.charAt(0));
    }

    private void sendKeyDown(int hit) {
        if (hit < 0) return;
        int r = hit / 20;
        int col = hit % 20;
        String baseCode = KEYS[r][col * 4 + 1];

        if (baseCode.equals("-1")) {           /* Fn */
            fnMode = !fnMode;
            shiftMode = false;
            shiftLock = false;
            ctrlLatch = altLatch = superLatch = false;
            updatePressedKey(hit);
            invalidate();
            return;
        }
        if (baseCode.equals("-2")) {           /* Shift：单击一次性，双击锁定 */
            long now = System.currentTimeMillis();
            if (now - lastShiftTap < 300) {
                shiftLock = !shiftLock;
                shiftMode = shiftLock;
            } else if (shiftMode && !shiftLock) {
                shiftMode = false;
                shiftLock = false;
            } else {
                shiftMode = true;
                shiftLock = false;
            }
            lastShiftTap = now;
            updatePressedKey(hit);
            invalidate();
            return;
        }

        int code = resolveCode(r, col, baseCode);

        /* 修饰键：粘滞(latch)，不直接发键；下次按普通键时带上 */
        if (code == 29)  { ctrlLatch  = !ctrlLatch;  updatePressedKey(hit); invalidate(); return; }
        if (code == 56)  { altLatch   = !altLatch;   updatePressedKey(hit); invalidate(); return; }
        if (code == 125) { superLatch = !superLatch; updatePressedKey(hit); invalidate(); return; }

        if (code == 58)                        /* Caps：本地记显示 */
            capsMode = !capsMode;

        /* 按固定顺序发：Shift, Ctrl, Alt, Super 的 down，再发键本身 */
        pressedMods = 0;
        if (shiftMode)  { Native.nativeSendKey(42,  true); pressedMods |= 1; }
        if (ctrlLatch)  { Native.nativeSendKey(29,  true); pressedMods |= 2; }
        if (altLatch)   { Native.nativeSendKey(56,  true); pressedMods |= 4; }
        if (superLatch) { Native.nativeSendKey(125, true); pressedMods |= 8; }
        Native.nativeSendKey(code, true);
        updatePressedKey(hit);
        invalidate();
    }

    /* 松开当前键：补发修饰键 up，并处理一次性 latch 的自动关闭 */
    private void releaseKey(int r, int col) {
        String baseCode = KEYS[r][col * 4 + 1];
        if (baseCode.equals("-1") || baseCode.equals("-2"))
            return;
        int code = resolveCode(r, col, baseCode);
        if (code == 29 || code == 56 || code == 125)   /* Ctrl/Alt/Super：松手不能清 latch */
            return;
        Native.nativeSendKey(code, false);
        if (pressedMods != 0) {
            if ((pressedMods & 1) != 0) Native.nativeSendKey(42,  false);
            if ((pressedMods & 2) != 0) Native.nativeSendKey(29,  false);
            if ((pressedMods & 4) != 0) Native.nativeSendKey(56,  false);
            if ((pressedMods & 8) != 0) Native.nativeSendKey(125, false);
            pressedMods = 0;
        }
        if (shiftMode && !shiftLock) shiftMode = false;   /* 一次性 Shift */
        ctrlLatch = altLatch = superLatch = false;        /* 一次性 latch */
        invalidate();
    }

    private void updatePressedKey(int hit) {
        pressedKey = hit;
    }

    private void clearPressedKey() {
        pressedKey = -1;
    }

    private int resolveCode(int r, int c, String baseCode) {
        if (fnMode && r == 0 && c * 2 < FN_ROW0.length && FN_ROW0[c * 2] != null) {
            return Integer.parseInt(FN_ROW0[c * 2 + 1]);
        }
        String shiftCode = KEYS[r][c * 4 + 3];
        if (shiftMode && shiftCode != null) {
            return Integer.parseInt(shiftCode);
        }
        return Integer.parseInt(baseCode);
    }

    private void cancelAllStates() {
        if (isDragging) {
            isDragging = false;
            dragPointerId = -1;
        }
        if (pressedKey >= 0) {
            releaseKey(pressedKey / 20, pressedKey % 20);
            clearPressedKey();
            invalidate();
        }
    }

    @Override
    public boolean onTouchEvent(MotionEvent event) {
        int action = event.getActionMasked();
        float x = event.getX();
        float y = event.getY();
        int touchPad = 20;

        if (action == MotionEvent.ACTION_CANCEL) {
            cancelAllStates();
            return true;
        }

        /* === Drag bar === */
        boolean onDragBar = x >= dragBarLeft && x <= dragBarRight
                && y >= dragBarY - touchPad && y <= dragBarY + BAR_W + touchPad;

        if (onDragBar) {
            switch (action) {
                case MotionEvent.ACTION_DOWN:
                    isDragging = false;
                    dragPointerId = event.getPointerId(0);
                    dragDownX = x;
                    dragDownY = y;
                    return true;
                case MotionEvent.ACTION_MOVE:
                {
                    int idx = event.findPointerIndex(dragPointerId);
                    if (idx < 0) return true;
                    float px = event.getX(idx), py = event.getY(idx);
                    if (!isDragging) {
                        float dist = (float) Math.hypot(px - dragDownX, py - dragDownY);
                        if (dist > dragSlop) {
                            isDragging = true;
                        }
                    }
                    if (isDragging) {
                        dragAccumX += px - dragDownX;
                        dragAccumY += py - dragDownY;
                        int dx = (int) dragAccumX;
                        int dy = (int) dragAccumY;
                        if (dx != 0 || dy != 0) {
                            offsetLeftAndRight(dx);
                            offsetTopAndBottom(dy);
                            dragAccumX -= dx;
                            dragAccumY -= dy;
                        }
                        dragDownX = px;
                        dragDownY = py;
                    }
                    return true;
                }
                case MotionEvent.ACTION_UP:
                    /* 横条只用来拖动位置：大小和透明度都不可调了，单击不再
                     * 展开任何额外控件。*/
                    isDragging = false;
                    dragPointerId = -1;
                    return true;
            }
        }

        /* If we were dragging and finger left the bar, keep dragging */
        if (isDragging) {
            switch (action) {
                case MotionEvent.ACTION_MOVE:
                {
                    int idx = event.findPointerIndex(dragPointerId);
                    if (idx < 0) return true;
                    float px = event.getX(idx), py = event.getY(idx);
                    dragAccumX += px - dragDownX;
                    dragAccumY += py - dragDownY;
                    int dx = (int) dragAccumX;
                    int dy = (int) dragAccumY;
                    if (dx != 0 || dy != 0) {
                        offsetLeftAndRight(dx);
                        offsetTopAndBottom(dy);
                        dragAccumX -= dx;
                        dragAccumY -= dy;
                    }
                    dragDownX = px;
                    dragDownY = py;
                    return true;
                }
                case MotionEvent.ACTION_UP:
                case MotionEvent.ACTION_POINTER_UP:
                    isDragging = false;
                    dragPointerId = -1;
                    return true;
                case MotionEvent.ACTION_CANCEL:
                    isDragging = false;
                    dragPointerId = -1;
                    return true;
            }
            return true;
        }

        /* === Key input (always active when not on controls) === */
        int hit = hitTest(x, y);
        if (hit >= 0) {
            switch (action) {
                case MotionEvent.ACTION_DOWN:
                    sendKeyDown(hit);
                    return true;
                case MotionEvent.ACTION_MOVE:
                    if (pressedKey >= 0 && hit != pressedKey) {
                        releaseKey(pressedKey / 20, pressedKey % 20);
                        clearPressedKey();
                        sendKeyDown(hit);
                    } else if (pressedKey < 0) {
                        sendKeyDown(hit);
                    }
                    return true;
                case MotionEvent.ACTION_UP:
                    if (pressedKey >= 0) {
                        releaseKey(pressedKey / 20, pressedKey % 20);
                        clearPressedKey();
                        invalidate();
                    }
                    return true;
                case MotionEvent.ACTION_POINTER_UP:
                    /* 第二根手指按着时抬起按住字母的那根，会走 POINTER_UP
                     * 而不是 UP；不处理的话 pressedKey 永远不释放，而键盘
                     * 声明了 EV_REP，容器里会一直自动重复。*/
                    if (event.getActionIndex() == 0 && pressedKey >= 0) {
                        releaseKey(pressedKey / 20, pressedKey % 20);
                        clearPressedKey();
                        invalidate();
                    }
                    return true;
            }
        } else {
            if (action == MotionEvent.ACTION_MOVE && pressedKey >= 0) {
                releaseKey(pressedKey / 20, pressedKey % 20);
                clearPressedKey();
                invalidate();
            }
        }

        return true;
    }
}