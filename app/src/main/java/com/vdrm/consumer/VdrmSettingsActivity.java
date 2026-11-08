package com.vdrm.consumer;

import android.app.Activity;
import android.content.SharedPreferences;
import android.os.Bundle;
import android.view.Gravity;
import android.view.ViewGroup;
import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.RadioButton;
import android.widget.RadioGroup;
import android.widget.ScrollView;
import android.widget.SeekBar;
import android.widget.Switch;
import android.widget.TextView;

/**
 * 长按桌面图标 → 「调试选项」打开的页面。
 *
 * 这里只有三个开关，都是"出问题时的逃生口"，默认值就是保守配置：
 *
 *   1. pace（flip-done 节拍）
 *        0 = 每个假 vblank tick 无条件发 flip-done（默认）。合成器不等
 *            SurfaceFlinger 的 present，帧率最高；代价是合成器可能在 SF 还在
 *            扫描时就开始画下一帧（表现为偶发闪/撕）。
 *        1 = 等到 App 报了这一帧的 present 才发。不撕，但会被 SF 串行化，
 *            帧率下降；present/release 不回时还可能把池子卡住。
 *        2 = 滞后一帧：这一帧 present 了、后面又有帧 present 才放行。
 *            合成器最多领先一帧，是 0 和 1 之间的折中。
 *
 *   2. in-flight（App 同一时刻允许几帧"还没上屏"）
 *        默认 4。调小更"跟手"但更容易被 SF 的 present 速度限住（1 会明显卡）；
 *        调大更平滑，但会让合成器领先屏幕更多。
 *
 *   3. 锁相环（pll）
 *        默认关。打开后 App 每 500ms 观测一次面板 vsync，把内核假 vblank
 *        的相位（tick_phase_ns）微调到"比面板 vsync 早约 2ms"，用来消除
 *        tick 与面板两个独立时钟之间的漂移。只在面板和合成器同频时才有意义
 *        （例如都是 120Hz）；不同频时不要开。
 *
 * 设置保存在 SharedPreferences("vdrm")，MainActivity 每次启动时应用一次。
 */
public class VdrmSettingsActivity extends Activity {

    public static final String PREF_PACE = "pace";
    public static final String PREF_INFLIGHT = "inflight";
    public static final String PREF_PLL = "pll";

    public static final int DEFAULT_PACE = 0;
    public static final int DEFAULT_INFLIGHT = 4;
    public static final boolean DEFAULT_PLL = false;

    private RadioGroup paceGroup;
    private SeekBar inflightBar;
    private TextView inflightLabel;
    private Switch pllSwitch;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);

        SharedPreferences sp = getSharedPreferences(MainActivity.PREFS, MODE_PRIVATE);

        LinearLayout root = new LinearLayout(this);
        root.setOrientation(LinearLayout.VERTICAL);
        int pad = (int) (20 * getResources().getDisplayMetrics().density);
        root.setPadding(pad, pad, pad, pad);

        root.addView(section("flip-done 节拍（pace）",
            "0 = 每个假 vblank 无条件发 flip-done：最流畅，但合成器可能重画正在扫描的显存（偶发闪/撕）。\n" +
            "1 = 等 App 报 present 才发：不撕，但会被 SF 串行化，帧率下降，release 不回时时可能卡住池子。\n" +
            "2 = 滞后一帧：这一帧 present、后面又有帧 present 才放行，折中。"));

        paceGroup = new RadioGroup(this);
        paceGroup.setOrientation(RadioGroup.VERTICAL);
        String[] paceNames = {"0 · tick（默认）", "1 · 等 present", "2 · 滞后一帧"};
        for (int i = 0; i < paceNames.length; i++) {
            RadioButton rb = new RadioButton(this);
            rb.setId(1000 + i);
            rb.setText(paceNames[i]);
            paceGroup.addView(rb);
        }
        int pace = sp.getInt(PREF_PACE, DEFAULT_PACE);
        paceGroup.check(1000 + Math.max(0, Math.min(2, pace)));
        root.addView(paceGroup);

        root.addView(section("in-flight（还没上屏的帧数上限）",
            "默认 4。调小更跟手，但更容易被 SF 的 present 速度限住（1 会明显卡顿）；" +
            "调大更平滑，但合成器会领先屏幕更多帧。"));

        inflightLabel = new TextView(this);
        inflightBar = new SeekBar(this);
        inflightBar.setMax(5);   /* 1..6 */
        int inflight = sp.getInt(PREF_INFLIGHT, DEFAULT_INFLIGHT);
        inflightBar.setProgress(Math.max(0, Math.min(5, inflight - 1)));
        inflightLabel.setText("当前：" + (inflightBar.getProgress() + 1));
        inflightBar.setOnSeekBarChangeListener(new SeekBar.OnSeekBarChangeListener() {
            @Override public void onProgressChanged(SeekBar seekBar, int progress, boolean fromUser) {
                inflightLabel.setText("当前：" + (progress + 1));
            }
            @Override public void onStartTrackingTouch(SeekBar seekBar) { }
            @Override public void onStopTrackingTouch(SeekBar seekBar) { }
        });
        root.addView(inflightLabel);
        root.addView(inflightBar);

        root.addView(section("锁相环（pll）",
            "默认关。打开后每 500ms 观测面板 vsync，把内核假 vblank 的相位" +
            "调到比面板 vsync 早约 2ms，用来消除两个独立时钟的漂移。\n" +
            "只在面板和合成器同频（如都是 120Hz）时才有意义；不同频时请保持关闭。"));

        pllSwitch = new Switch(this);
        pllSwitch.setText("启用锁相环");
        pllSwitch.setChecked(sp.getBoolean(PREF_PLL, DEFAULT_PLL));
        root.addView(pllSwitch);

        Button save = new Button(this);
        save.setText("保存并应用");
        save.setOnClickListener(new android.view.View.OnClickListener() {
            @Override public void onClick(android.view.View v) {
                int p = paceGroup.getCheckedRadioButtonId() - 1000;
                int n = inflightBar.getProgress() + 1;
                boolean pll = pllSwitch.isChecked();

                sp.edit()
                  .putInt(PREF_PACE, p)
                  .putInt(PREF_INFLIGHT, n)
                  .putBoolean(PREF_PLL, pll)
                  .apply();

                Native.nativeSetPace(p);
                Native.nativeSetInflight(n);
                Native.nativeSetPll(pll);
                finish();
            }
        });
        root.addView(save);

        ScrollView sv = new ScrollView(this);
        sv.addView(root);
        setContentView(sv);
    }

    private TextView section(String title, String body) {
        TextView tv = new TextView(this);
        tv.setText(title + "\n\n" + body);
        tv.setGravity(Gravity.START);
        int pad = (int) (12 * getResources().getDisplayMetrics().density);
        tv.setPadding(0, pad, 0, 0);
        tv.setTextSize(13);
        return tv;
    }

    /** MainActivity 启动时调一次，把上次保存的设置应用到 native 侧。 */
    public static void applySaved(Activity a) {
        SharedPreferences sp = a.getSharedPreferences(MainActivity.PREFS, MODE_PRIVATE);
        Native.nativeSetPace(sp.getInt(PREF_PACE, DEFAULT_PACE));
        Native.nativeSetInflight(sp.getInt(PREF_INFLIGHT, DEFAULT_INFLIGHT));
        Native.nativeSetPll(sp.getBoolean(PREF_PLL, DEFAULT_PLL));
    }
}
