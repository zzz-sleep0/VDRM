package com.vdrm.consumer;

import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.app.PendingIntent;
import android.app.RemoteInput;
import android.app.Service;
import android.content.Intent;
import android.graphics.drawable.Icon;
import android.os.Build;
import android.os.Bundle;
import android.os.IBinder;
import android.util.Log;

import java.io.BufferedReader;
import java.io.InputStreamReader;
import java.util.Locale;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;

/**
 * VDRM 的前台服务，同时也是【配置面】。
 *
 * 为什么把配置放在通知里而不是界面上：容器占着整块屏，App 的 Activity 只能
 * 缩在窗口里当 letterbox，平时根本碰不到；而通知栏是唯一随时可达、又不需要
 * 自己造一套 IPC/权限模型的地方（不新开 socket、不新增节点、SELinux 不用管）。
 * 形式照 Shizuku 那套：一个常驻通知 + 几个文字动作按钮 + 一个 RemoteInput
 * 文本入口，状态机就是"同一 id 反复 notify 覆盖"。
 *
 * 两个动作：
 *   刷新     重新读一遍内核参数（档位可能是合成器改的，不是 App 改的）
 *   命令     RemoteInput，格式是【符号 + 参数】，见 runCommand()
 *
 * **分辨率/刷新率/DPI 都不是 App 的事了**：模式列表由驱动广告，合成器自己挑、
 * 自己 modeset；那一档还没有池槽时驱动发 `want_mode`，App 补槽（native 的
 * ensure_pool_spawn）。所以通知里没有"切档/切横竖"按钮 —— 用合成器的工具切
 * （kscreen-doctor / niri msg output / hyprctl），App 只负责补槽和显示状态。
 *
 * 为什么命令和状态都走 su：内核参数在 /sys/module 下，普通 App 读不到也写
 * 不了。App 本来就已经用 su 拉 mkpool（见 native 的 ensure_pool_spawn），
 * 复用它就够了，不需要新的权限模型。
 *
 * 这一版刻意做小：
 *   - 只有一个 channel、一条通知，不分"状态/干活"两档；
 *   - 不轮询：只在启动、收到命令、以及 App 回到前台时刷新；
 *   - 命令只留 2 条（input / commit）；scale / refresh / reload / 日志抓取都没进来。
 */
public class ForegroundService extends Service {

    private static final String TAG = "VDRM";
    private static final String CHANNEL_ID = "vdrm";
    private static final int NOTIF_ID = 1001;

    /* onStartCommand 的 action —— PendingIntent 只能带字符串，所以就靠它分派 */
    private static final String ACT_REPLY   = "com.vdrm.consumer.REPLY";
    private static final String ACT_REFRESH = "com.vdrm.consumer.REFRESH";

    /** RemoteInput 的 key，取结果时要用同一个 */
    private static final String KEY_CMD = "cmd";

    private static final String P = "/sys/module/vdrm/parameters";

    /** su 一律串行跑：并发 exec 三条 su 会把输入法/系统 su 服务搞到超时 */
    private final ExecutorService bg = Executors.newSingleThreadExecutor();

    /* 通知的两个可变部分。volatile：bg 线程写，主线程读 */
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
            /* 拿不到前台（极少见）：退化成普通通知，服务照样活 */
            Log.w(TAG, "startForeground failed", e);
            getSystemService(NotificationManager.class).notify(NOTIF_ID, build());
        }
        refresh();
    }

    @Override
    public int onStartCommand(Intent intent, int flags, int startId) {
        String action = intent != null ? intent.getAction() : null;

        if (action == null || ACT_REFRESH.equals(action)) {
            refresh();
        } else if (ACT_REPLY.equals(action)) {
            String cmd = readReply(intent);
            bg.execute(() -> runCommand(cmd));
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
        /* 点正文 = 打开 App。不光是便利：如果哪个 OEM 把 inline reply 的
         * 输入框藏了，"命令"按钮就会看起来没反应 —— 至少还有这条路能进来。*/
        Intent open = new Intent(this, MainActivity.class)
                .addFlags(Intent.FLAG_ACTIVITY_NEW_TASK | Intent.FLAG_ACTIVITY_SINGLE_TOP);
        PendingIntent content = PendingIntent.getActivity(this, 0, open, piFlags());

        Notification.Builder b = new Notification.Builder(this, CHANNEL_ID)
                .setSmallIcon(android.R.drawable.ic_menu_manage)
                .setContentTitle(nTitle)
                .setContentText(nText)
                .setContentIntent(content)
                .setOngoing(true)          /* 常驻：它同时是服务活着的证据 */
                .setOnlyAlertOnce(true)    /* 状态刷新不响铃 */
                .setShowWhen(false)
                /* 动作图标传 null：纯文字按钮，和 Shizuku 一样。
                 * 只留两个：切档交给合成器，这里不再有"全面屏/切横竖"。*/
                .addAction(new Notification.Action.Builder(
                        (Icon) null, "刷新", actionPi(ACT_REFRESH)).build())
                .addAction(new Notification.Action.Builder(
                        (Icon) null, "命令", replyPi())
                        .addRemoteInput(replyInput()).build());

        /* 不用 subText：通知的副标题是单行、窄屏上直接变成省略号，
         * 看不全等于没有。正文 = 上一条命令的结果（没有结果时就是"就绪"），
         * 标题 = 当前档位 + 刷新率。*/
        return b.build();
    }

    /** 发版：同一 id 覆盖，不新建 */
    private void post() {
        NotificationManager nm = getSystemService(NotificationManager.class);
        Notification n = build();

        try {
            nm.notify(NOTIF_ID, n);
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

    /** 带 RemoteInput 的 PendingIntent **必须**是 MUTABLE（API 31+），
     *  否则系统塞不进输入结果，取出来永远是空。*/
    private PendingIntent replyPi() {
        Intent i = new Intent(this, ForegroundService.class).setAction(ACT_REPLY);
        int flags = PendingIntent.FLAG_UPDATE_CURRENT;
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S)
            flags |= PendingIntent.FLAG_MUTABLE;
        else if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.M)
            flags |= PendingIntent.FLAG_IMMUTABLE;
        return PendingIntent.getService(this, 2, i, flags);
    }

    /**
     * 输入框的灰字提示：放**当前配置本身**的可执行写法 —— 想改就把那个
     * 数字改掉，不用记格式。
     */
    private RemoteInput replyInput() {
        int mode = getSharedPreferences(MainActivity.PREFS, MODE_PRIVATE)
                .getInt(MainActivity.PREF_INPUT, MainActivity.INPUT_TOUCH);
        String hint = "input " + (mode == MainActivity.INPUT_TOUCH ? 1 : 0);

        return new RemoteInput.Builder(KEY_CMD).setLabel(hint).build();
    }

    private static String readReply(Intent intent) {
        if (intent == null)
            return "";
        Bundle r = RemoteInput.getResultsFromIntent(intent);
        if (r == null)
            return "";
        CharSequence s = r.getCharSequence(KEY_CMD);
        return s == null ? "" : s.toString().trim();
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

    /**
     * 一次 su 把想显示的读出来（一次 exec 比十次便宜，也少十次风控）。
     * 不轮询：调用点只有 onCreate / 命令结束后 / Activity 回前台。
     */
    private void refresh() {
        if (refreshing)
            return;
        refreshing = true;
        bg.execute(() -> {
            try {
                String q = "P=" + P + "; "
                        + "echo m=$(cat $P/mode); "
                        + "echo r=$(cat $P/refresh)";
                String out = sh(q);

                if (out.startsWith("su 不可用")) {
                    nTitle = "VDRM";
                    nText = out;
                } else {
                    String m = field(out, "m"), r = field(out, "r");

                    if (m.isEmpty()) {
                        nTitle = "VDRM";
                        nText = "模块没加载？";
                    } else {
                        nTitle = m + "@" + r;
                        if (nText.isEmpty() || nText.startsWith("正在"))
                            nText = "就绪";
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

    /* ==================== 命令 ==================== */

    /**
     * 【符号 + 参数】。只有这个白名单里的会被执行 —— 用户输入永远不会被拼进
     * shell 命令里，除了经过校验的数字。
     *
     *   input 0|1     0 = 鼠标（触控板相对位移），1 = 触控屏（直接触摸）
     *   commit        只广播（手改了 sysfs 参数、想让它生效时用）
     *
     * 为什么没有 mode / refresh：这两样现在都是**合成器**的事 —— 驱动把
     * 档位（`refresh`/120/90/60、加上真实分辨率及竖版）全部广告进模式列表，
     * 合成器用 `kscreen-doctor` / `niri msg output` / `hyprctl` 自己挑；
     * 缺槽时 App 补槽（native 的 ensure_pool_spawn）。App 再从通知里抢先改
     * `mode`，就会和合成器打架（合成器要的那一档刚建好就被判 stale）。
     *
     * 为什么没有 scale：缩放是合成器的属性（wl_output.scale、字体/光标尺寸、
     * XWayland 缩放都跟着它走），DRM 设备表达不了，跨合成器也没有统一接口。
     * 让用户在自己的合成器配置里写一次即可（README 有每家的写法）。
     */
    private void runCommand(String cmd) {
        if (cmd.isEmpty()) {
            nText = "空命令：input 0|1 / commit";
            post();
            return;
        }
        String[] t = cmd.split("\\s+");
        String sym = t[0].toLowerCase(Locale.ROOT);
        String arg = t.length > 1 ? t[1] : "";
        String res;

        switch (sym) {
            case "input":
                res = setInput(arg);
                break;
            case "commit":
                res = commit() ? "commit ✔" : "commit 失败";
                break;
            default:
                res = "未知符号: " + sym;
                break;
        }
        nText = res;
        post();
        refresh();
    }

    /**
     * 输入模式：手机触摸当鼠标还是当触控屏。
     *
     * 两个设备内核都注册好了（REL 鼠标 + ABS_MT 触控屏），所以这里不用动内核，
     * 只是告诉 App"下一次触摸按哪种方式注入"：
     *   0 = 鼠标    手指拖动 → 相对位移（触控板手感，指针停在原地不跳）
     *   1 = 触控屏  按哪点哪 → 绝对坐标
     *
     * 切换时先把还在按着的触点全部抬起，免得从触屏切到鼠标时留一个"按住"。
     */
    private String setInput(String arg) {
        if (!arg.equals("0") && !arg.equals("1"))
            return "input 只认 0（鼠标）或 1（触控屏）";

        int mode = Integer.parseInt(arg);

        MainActivity.inputMode = mode;
        getSharedPreferences(MainActivity.PREFS, MODE_PRIVATE)
                .edit().putInt(MainActivity.PREF_INPUT, mode).apply();
        /* 抬掉所有触点（count=0 就是全部抬起） */
        Native.nativeSendTouch(0, new int[0], new float[0],
                               new float[0], new float[0]);
        return mode == MainActivity.INPUT_TOUCH ? "input 触控屏 ✔" : "input 鼠标 ✔";
    }

    private boolean commit() {
        return sh("echo 1 > " + P + "/commit").isEmpty();
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
