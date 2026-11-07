package com.vdrm.consumer;

import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.app.PendingIntent;
import android.app.Service;
import android.content.Intent;
import android.graphics.drawable.Icon;
import android.os.Build;
import android.os.IBinder;
import android.util.Log;

import java.io.BufferedReader;
import java.io.InputStreamReader;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;

/**
 * VDRM 的前台服务，同时也是【配置面】。
 *
 * 容器占着整块屏，App 的 Activity 缩在窗口里平时碰不到；通知栏是唯一随时可达、
 * 又不用另造 IPC/权限模型的地方。所以控制面就是这条常驻通知。
 *
 * 三个按钮，都是一键，没有输入框：
 *   鼠标   手机触摸当鼠标（相对位移，触控板手感）
 *   触控   手机触摸当触控屏（按哪点哪）
 *   刷新   重读内核参数（档位可能是合成器改的，不是 App 改的）
 *
 * 分辨率/刷新率/缩放不在这里：档位由驱动广告、合成器自己挑（kscreen-doctor /
 * niri msg / swaymsg），缺槽时 App 的 native 侧按 want_gen 自动补槽。
 */
public class ForegroundService extends Service {

    private static final String TAG = "VDRM";
    private static final String CHANNEL_ID = "vdrm";
    private static final int NOTIF_ID = 1001;

    private static final String ACT_MOUSE   = "com.vdrm.consumer.MOUSE";
    private static final String ACT_TOUCH   = "com.vdrm.consumer.TOUCH";
    private static final String ACT_REFRESH = "com.vdrm.consumer.REFRESH";

    private static final String P = "/sys/module/vdrm/parameters";

    /** su 一律串行跑：并发 exec 多条 su 会把输入法/系统 su 服务搞到超时 */
    private final ExecutorService bg = Executors.newSingleThreadExecutor();

    private volatile String nTitle = "VDRM";
    private volatile String nText  = "正在读取状态…";
    private volatile boolean refreshing;

    @Override
    public void onCreate() {
        super.onCreate();

        NotificationChannel ch = new NotificationChannel(
                CHANNEL_ID, "VDRM", NotificationManager.IMPORTANCE_LOW);
        ch.setShowBadge(false);
        ch.setSound(null, null);
        getSystemService(NotificationManager.class).createNotificationChannel(ch);

        try {
            startForeground(NOTIF_ID, build());
        } catch (Exception e) {
            Log.w(TAG, "startForeground failed", e);
            getSystemService(NotificationManager.class).notify(NOTIF_ID, build());
        }
        refresh();
    }

    @Override
    public int onStartCommand(Intent intent, int flags, int startId) {
        String action = intent != null ? intent.getAction() : null;

        if (ACT_MOUSE.equals(action)) {
            bg.execute(() -> setInput(MainActivity.INPUT_MOUSE));
        } else if (ACT_TOUCH.equals(action)) {
            bg.execute(() -> setInput(MainActivity.INPUT_TOUCH));
        } else {
            refresh();
        }
        return START_STICKY;
    }

    @Override
    public IBinder onBind(Intent intent) {
        return null;
    }

    @Override
    public void onDestroy() {
        bg.shutdownNow();
        super.onDestroy();
    }

    /* ==================== 通知 ==================== */

    private Notification build() {
        Intent open = new Intent(this, MainActivity.class)
                .addFlags(Intent.FLAG_ACTIVITY_NEW_TASK | Intent.FLAG_ACTIVITY_SINGLE_TOP);
        PendingIntent content = PendingIntent.getActivity(this, 0, open, piFlags());

        Notification.Builder b = new Notification.Builder(this, CHANNEL_ID)
                .setSmallIcon(android.R.drawable.ic_menu_manage)
                .setContentTitle(nTitle)
                .setContentText(nText)
                .setContentIntent(content)
                .setOngoing(true)
                .setOnlyAlertOnce(true)
                .setShowWhen(false)
                .addAction(new Notification.Action.Builder(
                        (Icon) null, "鼠标", actionPi(ACT_MOUSE)).build())
                .addAction(new Notification.Action.Builder(
                        (Icon) null, "触控", actionPi(ACT_TOUCH)).build())
                .addAction(new Notification.Action.Builder(
                        (Icon) null, "刷新", actionPi(ACT_REFRESH)).build());

        return b.build();
    }

    private void post() {
        NotificationManager nm = getSystemService(NotificationManager.class);

        try {
            nm.notify(NOTIF_ID, build());
        } catch (Exception e) {
            Log.w(TAG, "notify failed", e);
        }
    }

    private static int piFlags() {
        int flags = PendingIntent.FLAG_UPDATE_CURRENT;
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.M)
            flags |= PendingIntent.FLAG_IMMUTABLE;
        return flags;
    }

    private PendingIntent actionPi(String action) {
        Intent i = new Intent(this, ForegroundService.class).setAction(action);
        return PendingIntent.getService(this, action.hashCode(), i, piFlags());
    }

    /** 外部（Activity 回到前台）触发的刷新：不新建服务，只发 action */
    public static void requestRefresh(android.content.Context ctx) {
        try {
            Intent i = new Intent(ctx, ForegroundService.class).setAction(ACT_REFRESH);
            ctx.startService(i);
        } catch (Exception ignored) {
        }
    }

    /* ==================== 状态 ==================== */

    private void refresh() {
        if (refreshing)
            return;
        refreshing = true;
        bg.execute(() -> {
            try {
                String q = "P=" + P + "; "
                        + "echo m=$(cat $P/mode); "
                        + "echo r=$(cat $P/refresh); "
                        + "echo a=$(cat $P/active)";
                String out = sh(q);

                if (out.startsWith("su 不可用")) {
                    nTitle = "VDRM";
                    nText = out;
                } else {
                    String m = field(out, "m"), r = field(out, "r");
                    String a = field(out, "a");

                    if (m.isEmpty()) {
                        nTitle = "VDRM";
                        nText = "模块没加载？";
                    } else {
                        nTitle = (a.isEmpty() ? m : a) + "@" + r;
                        nText = inputLabel();
                    }
                }
            } finally {
                refreshing = false;
                post();
            }
        });
    }

    /** 从 "m=1920x1080 r=120 …" 里取一个值 */
    private static String field(String out, String key) {
        for (String tok : out.split("\\s+")) {
            int eq = tok.indexOf('=');
            if (eq > 0 && tok.substring(0, eq).equals(key))
                return tok.substring(eq + 1).trim();
        }
        return "";
    }

    private String inputLabel() {
        int mode = getSharedPreferences(MainActivity.PREFS, MODE_PRIVATE)
                .getInt(MainActivity.PREF_INPUT, MainActivity.INPUT_TOUCH);

        return mode == MainActivity.INPUT_TOUCH ? "输入：触控屏" : "输入：鼠标";
    }

    /* ==================== 动作 ==================== */

    /**
     * 输入模式：手机触摸当鼠标还是当触控屏。
     *
     * 两个设备内核都注册好了（REL 鼠标 + ABS_MT 触控屏），所以这里不用动内核，
     * 只是告诉 App"下一次触摸按哪种方式注入"。切换时先把还在按着的触点全部抬起，
     * 免得从触屏切到鼠标时留一个"按住"。
     */
    private void setInput(int mode) {
        MainActivity.inputMode = mode;
        getSharedPreferences(MainActivity.PREFS, MODE_PRIVATE)
                .edit().putInt(MainActivity.PREF_INPUT, mode).apply();
        Native.nativeSendTouch(0, new int[0], new float[0],
                               new float[0], new float[0]);
        nText = (mode == MainActivity.INPUT_TOUCH) ? "输入：触控屏 ✔" : "输入：鼠标 ✔";
        post();
    }

    /* ==================== su ==================== */

    /** 跑一次 su -c，返回输出（合并 stderr，压成一行）。空字符串 = 成功且无输出 */
    private static String sh(String cmd) {
        Process p = null;
        try {
            p = new ProcessBuilder("su", "-c", cmd)
                    .redirectErrorStream(true).start();
            StringBuilder sb = new StringBuilder();
            try (BufferedReader r = new BufferedReader(
                    new InputStreamReader(p.getInputStream()))) {
                String l;
                while ((l = r.readLine()) != null) {
                    if (sb.length() < 300)
                        sb.append(l.trim()).append(' ');
                }
            }
            p.waitFor();
            return sb.toString().trim();
        } catch (Exception e) {
            return "su 不可用: " + e.getMessage();
        } finally {
            if (p != null)
                p.destroy();
        }
    }
}
