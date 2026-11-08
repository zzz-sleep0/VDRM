package com.vdrm.consumer;

import android.app.Activity;
import android.content.SharedPreferences;
import android.os.Bundle;
import android.view.Gravity;
import android.view.ViewGroup;
import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.SeekBar;
import android.widget.Switch;
import android.widget.TextView;

/**
 * 长按桌面图标 → 「调试选项」打开的页面。
 *
 * 这里只有两个开关，都是"出问题时的逃生口"，默认值就是保守配置：
 *
 *   1. in-flight（App 同一时刻允许几帧"还没上屏"）
 *        默认 4。调小更"跟手"但更容易被 SF 的 present 速度限住（1 会明显卡）；
 *        调大更平滑，但会让合成器领先屏幕更多。
 *
 *   2. 锁相环（pll）
 *        默认关。打开后 App 每 500ms 观测一次面板 vsync，把内核假 vblank
 *        的相位（tick_phase_ns）微调到"比面板 vsync 早约 2ms"，用来消除
 *        tick 与面板两个独立时钟之间的漂移。只在面板和合成器同频时才有意义
 *        （例如都是 120Hz）；不同频时不要开。
 *
 * 设置保存在 SharedPreferences("vdrm")，MainActivity 每次启动时应用一次。
 */
public class VdrmSettingsActivity extends Activity {

    public static final String PREF_INFLIGHT = "inflight";
    public static final String PREF_PLL = "pll";

    public static final int DEFAULT_INFLIGHT = 4;
    public static final boolean DEFAULT_PLL = false;

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

        root.addView(section("在飞帧数（in-flight）",
            "默认 4。\n" +
            "调大：画面更平滑，但合成器会领先屏幕更多帧，延迟变大。\n" +
            "调小：更跟手、延迟更小，但容易被上屏速度限住，太小（1）会明显卡顿。"));

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
            "默认关。\n" +
            "打开后每 500 毫秒观测一次面板 vsync，把内核假 vblank 的相位调到比面板 vsync 早约 2 毫秒，" +
            "用来消除两个独立时钟之间的漂移。\n" +
            "只在面板和合成器同频（比如都是 120Hz）时有用；不同频时请保持关闭。"));

        pllSwitch = new Switch(this);
        pllSwitch.setText("启用锁相环");
        pllSwitch.setChecked(sp.getBoolean(PREF_PLL, DEFAULT_PLL));
        root.addView(pllSwitch);

        Button save = new Button(this);
        save.setText("保存并应用");
        save.setOnClickListener(new android.view.View.OnClickListener() {
            @Override public void onClick(android.view.View v) {
                int n = inflightBar.getProgress() + 1;
                boolean pll = pllSwitch.isChecked();

                sp.edit()
                  .putInt(PREF_INFLIGHT, n)
                  .putBoolean(PREF_PLL, pll)
                  .apply();

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
        Native.nativeSetInflight(sp.getInt(PREF_INFLIGHT, DEFAULT_INFLIGHT));
        Native.nativeSetPll(sp.getBoolean(PREF_PLL, DEFAULT_PLL));
    }
}
