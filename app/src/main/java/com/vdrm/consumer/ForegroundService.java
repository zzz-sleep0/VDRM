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
 * 三个动作：
 *   切横竖   16:9 两档之间切（和模块 Action 键的效果一致）
 *   重建池   mkpool --reset（池被耗光/换过档之后的手动救）
 *   命令     RemoteInput，格式是【符号 + 参数】，见 runCommand()
 *
 * 为什么命令和状态都走 su：内核参数在 /sys/module 下，普通 App 读不到也写
 * 不了。App 本来就已经用 su 拉 mkpool（见 native 的 ensure_pool_spawn），
 * 复用它就够了，不需要新的权限模型。
 *
 * 这一版刻意做小（先看形状，之后再砍）：
 *   - 只有一个 channel、一条通知，不分"状态/干活"两档；
 *   - 不轮询：只在启动、收到命令、以及 App 回到前台时刷新；
 *   - 命令只留 6 条；reload / niri 转发 / 日志抓取都没进来。
 */
public class ForegroundService extends Service {

    private static final String TAG = "VDRM";
    private static final String CHANNEL_ID = "vdrm";
    private static final int NOTIF_ID = 1001;

    /* onStartCommand 的 action —— PendingIntent 只能带字符串，所以就靠它分派 */
    private static final String ACT_PRESET  = "com.vdrm.consumer.PRESET";   /* 黑边/全面屏 互相切 */
    private static final String ACT_SWAP    = "com.vdrm.consumer.SWAP";     /* 横竖互换 */
    private static final String ACT_REPLY   = "com.vdrm.consumer.REPLY";
    private static final String ACT_REFRESH = "com.vdrm.consumer.REFRESH";

    /** RemoteInput 的 key，取结果时要用同一个 */
    private static final String KEY_CMD = "cmd";

    private static final String MODDIR = "/data/adb/modules/vdrm-lkm";
    /* 容器名和 droidspaces 路径：改了容器名要一起改。只有 scale 用到它们 ——
     * UI 缩放是合成器的概念，只能通过 niri 的 IPC 调。*/
    private static final String CT = "Arch-dev";
    private static final String DS = "/data/local/Droidspaces/bin/droidspaces";
    private static final String P = "/sys/module/vdrm/parameters";

    /** su 一律串行跑：并发 exec 三条 su 会把输入法/系统 su 服务搞到超时 */
    private final ExecutorService bg = Executors.newSingleThreadExecutor();

    /* 通知的三个可变部分。volatile：bg 线程写，主线程读 */
    private volatile String nTitle = "VDRM";
    private volatile String nText  = "正在读取状态…";

    /* 当前 mode 参数 / 两个预置档的尺寸（"2247x1264" 这种）。
     * nMode 来自每次 refresh，两个预置来自 `wm size`，开机算一次就够。*/
    private volatile String nMode = "";
    private volatile String nRefresh = "";     /* 当前刷新率，输入框提示用 */
    private volatile String nScale   = "";     /* 当前合成器 scale，输入框提示用 */
    private volatile String nFull = "";        /* 全面屏那一档（真实横屏尺寸） */

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
        } else if (ACT_PRESET.equals(action)) {
            bg.execute(this::applyNextPreset);
        } else if (ACT_SWAP.equals(action)) {
            bg.execute(this::swapOrientation);
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
                 * 三个按钮刚好是通知上限。*/
                /* 标签随当前档变：现在全面屏就显示"黑边屏"（点了切过去），反之亦然。
                 * 这样一个按钮顶两个，第三个位置还能留给"切横竖"。*/
                .addAction(new Notification.Action.Builder(
                        (Icon) null, presetLabel(), actionPi(ACT_PRESET)).build())
                .addAction(new Notification.Action.Builder(
                        (Icon) null, "切横竖", actionPi(ACT_SWAP)).build())
                .addAction(new Notification.Action.Builder(
                        (Icon) null, "命令", replyPi())
                        .addRemoteInput(replyInput()).build());

        /* 不再用 subText：通知的副标题是单行、窄屏上直接变成省略号，
         * 看不全等于没有。状态改成放在正文里（正文=上一条命令结果，
         * 没结果时就是"就绪"），标题是当前档位。*/
        return b.build();
    }

    /** 按钮上该显示"下一个档"的名字 */
    private String presetLabel() {
        String full = nFull;

        if (full.isEmpty() || nMode.isEmpty())
            return "全面屏";
        return isFullMode(nMode, full) ? "黑边屏" : "全面屏";
    }

    /** 当前是不是"全面屏"那一档（含它的竖屏互换形式） */
    private static boolean isFullMode(String mode, String full) {
        return mode.equals(full) || mode.equals(swap(full));
    }

    private static String swap(String wh) {
        int x = wh.indexOf('x');

        if (x <= 0)
            return wh;
        return wh.substring(x + 1) + "x" + wh.substring(0, x);
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
     * 输入框的灰字提示（原本说的"淡化行"）。
     *
     * 放的是**当前配置本身**，而且是可直接执行的写法 —— 想改哪个数字，
     * 照着把光标挪过去改一下就行，不用记格式：
     *
     *     input 1 · refresh 120 · scale 2.0
     *
     * 所以它既是状态，又是模板。以前这里写的是 `input 0|1 · refresh 90 …`
     * 那种"格式说明"，每台机器都一样、没有信息量。
     */
    private RemoteInput replyInput() {
        int mode = getSharedPreferences(MainActivity.PREFS, MODE_PRIVATE)
                .getInt(MainActivity.PREF_INPUT, MainActivity.INPUT_TOUCH);
        StringBuilder sb = new StringBuilder("input ")
                .append(mode == MainActivity.INPUT_TOUCH ? 1 : 0);

        if (!nRefresh.isEmpty())
            sb.append(" · refresh ").append(nRefresh);
        if (!nScale.isEmpty())
            sb.append(" · scale ").append(nScale);

        return new RemoteInput.Builder(KEY_CMD).setLabel(sb.toString()).build();
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
     * 一次 su 把所有想显示的读出来（一次 exec 比十次便宜，也少十次风控）。
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
                        + "echo r=$(cat $P/refresh); "
                        + "wm size";   /* 顺带算两个预置档，省一次 su */
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
                        nMode = m;          /* 按钮标签靠它决定 */
                        nRefresh = r;
                        int[] rs = parseRealSize(out);

                        if (rs != null)
                            nFull = rs[0] + "x" + rs[1];
                        /* 标题只放"档"；状态全部收进淡化那行（subText），
                         * 正文留给上一条命令的结果 —— 这样不用单独一条
                         * `status` 命令，通知本身就一直是当前状态。*/
                        nTitle = m + "@" + r;
                        if (nText.isEmpty() || nText.startsWith("正在"))
                            nText = "就绪";
                        /* 输入框提示要"当前值 + 可直接照着改"，所以 scale 也得读
                         * 一次（只在刷新时读，不是每帧）。*/
                        nScale = readScale();
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

    /* ==================== 动作 ==================== */

    /**
     * 两个预置档，和模块 Action 键（module/action.sh）算的是同一对数：
     *
     *   黑边    = 真实尺寸里能放下的最大 16:9（居中、两侧留黑边、避开挖孔）
     *   全面屏  = 真实横屏尺寸（铺满，挖孔会压在桌面上）
     *
     * 都不写死：真实尺寸从 `wm size` 来（Override 优先、竖屏转横屏），
     * 换台机器自动跟着变。1920x1080 只是 .ko 的兜底默认。
     *
     * **保持当前方向**：现在是竖的就切竖的那一档，不硬跳回横屏。
     */
    private void applyNextPreset() {
        int[] sz = realSize();
        if (sz == null) {
            nText = "读不到屏幕尺寸（wm size 失败）";
            post();
            return;
        }
        int w = sz[0], h = sz[1];
        /* 黑边档用固定 1920x1080，不按"能放下的最大 16:9"去算（本机会算出
         * 2247x1264）。原因很实在：它的竖版 1264x2247 在这块 GPU 上导不进
         * dmabuf（niri: Failed to convert between dmabuf and EGLImage），
         * 而 1080x1920 是确定能上屏的 —— 两档都要能横竖切，就得用这个。
         * 1920x1080 本来也是 .ko 的兜底默认，不算新写死的数。*/
        String safe = "1920x1080";
        String full = w + "x" + h;

        nFull = full;

        boolean toSafe = isFullMode(nMode, full);
        String want = toSafe ? safe : full;
        boolean portrait = isPortrait(nMode);

        /* 现在竖着就切竖的那一档（宽高互换），不要因为"预设是按横屏算的"
         * 就把屏幕掰回横屏 —— 那正是"切黑边屏却先跳到横屏"的由来。*/
        if (portrait)
            want = swap(want);

        applyTarget(want, (toSafe ? "黑边 " : "全面屏 ") + (portrait ? "(竖) " : ""));
    }

    /** 横竖互换：把当前档的宽高对调（两档预置的竖版都是能上屏的 1080x1920 /
     *  1264x2780；就算切到别的尺寸，applyTarget 也会给出"没生效"的明确提示）*/
    private void swapOrientation() {
        String cur = nMode.isEmpty() ? sh("cat " + P + "/mode").trim() : nMode;
        String want = swap(cur);

        if (want.equals(cur) || want.indexOf('x') <= 0) {
            nText = "读不到当前分辨率，无法互换";
            post();
            return;
        }
        applyTarget(want, isPortrait(want) ? "竖屏 " : "横屏 ");
    }

    private static boolean isPortrait(String wh) {
        int x = wh.indexOf('x');

        if (x <= 0)
            return false;
        try {
            return Integer.parseInt(wh.substring(0, x))
                    < Integer.parseInt(wh.substring(x + 1));
        } catch (Exception e) {
            return false;
        }
    }

    /**
     * 设档 + **验证**：写完 commit 之后回读 `active`（合成器真正编进 pipe 的
     * 尺寸）。只看 mode 参数会以为成功 —— 合成器可能因为缓冲导入失败、热插拔
     * 没跟而原地不动（本机实测某些尺寸在 niri/mesa 里 import 失败，
     * 日志是 "Failed to convert between dmabuf and EGLImage"）。
     */
    private void applyTarget(String want, String what) {
        nText = "切" + what + want + " …";
        post();

        String r = runMkpool(want);

        if (!r.endsWith("✔")) {
            nText = r;
            post();
            refresh();
            return;
        }
        if (waitActive(want)) {
            persist("mode", want);      /* 记住，重启别丢 */
            nText = "已切到 " + want + " ✔";
            post();
            refresh();
            return;
        }

        /* 竖版没上屏时退回横版再试一次。本机实测：这块 GPU 导不进某些尺寸的
         * linear dmabuf（niri 日志 "Failed to convert between dmabuf and
         * EGLImage"），典型是竖着的 16:9 黑边档（1264x2247）。与其让用户看着
         * "点了没反应"，不如退到能用的那一档并说明白。*/
        String alt = swap(want);

        if (!alt.equals(want)) {
            nText = "切 " + alt + " …";
            post();
            if (runMkpool(alt).endsWith("✔") && waitActive(alt)) {
                nText = want + " 上不了屏（GPU 导不进这个尺寸），已切 " + alt;
                post();
                refresh();
                return;
            }
        }
        String act = sh("cat " + P + "/active").trim();
        nText = want + " 没生效（屏幕仍是 " + (act.isEmpty() ? "?" : act) + "）";
        post();
        refresh();
    }

    /** 最多等 3 秒，看合成器有没有真的把 active 换成 want */
    private boolean waitActive(String want) {
        for (int i = 0; i < 10; i++) {
            sleep(300);
            if (sh("cat " + P + "/active").trim().equals(want))
                return true;
        }
        return false;
    }

    private static void sleep(long ms) {
        try {
            Thread.sleep(ms);
        } catch (InterruptedException e) {
            Thread.currentThread().interrupt();
        }
    }

    /**
     * `wm size` → 横屏尺寸 {w,h}；拿不到返回 null。
     *
     * 输出形如 "Physical size: 1264x2780"（可能还有 "Override size: …"）。
     * 注意 sh() 会把多行压成一行，所以这里按正则扫、取**最后**一个匹配 ——
     * Override 总在 Physical 之后，正好就是想要的优先级。
     */
    private int[] realSize() {
        return parseRealSize(sh("wm size"));
    }

    /** 从任意文本里扫 "size: WxH"（取最后一个匹配），返回横屏 {w,h} */
    private static int[] parseRealSize(String out) {
        java.util.regex.Matcher m = java.util.regex.Pattern
                .compile("size:\\s*(\\d{2,5})x(\\d{2,5})").matcher(out);
        int w = 0, h = 0;

        while (m.find()) {
            try {
                w = Integer.parseInt(m.group(1));
                h = Integer.parseInt(m.group(2));
            } catch (Exception e) {
                w = h = 0;
            }
        }
        if (w < 16 || h < 16)
            return null;
        return new int[]{ Math.max(w, h), Math.min(w, h) };   /* 横屏 */
    }

    /* ==================== 命令 ==================== */

    /**
     * 【符号 + 参数】。只有这个白名单里的会被执行 —— 用户输入永远不会被拼进
     * shell 命令里，除了经过校验的数字和 WxH。
     *
     *   input 0|1     0 = 鼠标（触控板相对位移），1 = 触控屏（直接触摸）
     *   refresh N     24..240，写参数 + commit（实时生效）
     *   scale N       0.5..3.0，改合成器的 scale（实时生效 —— 这才是"UI 变大变小"）
     *   commit        只广播（改完参数不想重建池时用）
     *
     * 为什么不放 dpi：`edid_dpi` 只改 EDID 里的物理尺寸，合成器（niri）的
     * scale 是它自己配置里的值，不会因为 EDID 变了就重算 —— 改完屏幕上毫无
     * 变化，看起来就是"假的"。它只在合成器**启动前**有意义，所以留在
     * mode.conf 的 `dpi=` 里，不进通知。
     *
     * 分辨率不进这里：日常只有"黑边/全面屏"两档，做成按钮了。任意 WxH 现在
     * 内核侧也支持（第 48 项），但没有场景，留着只会让人以为这是主要入口。
     */
    private void runCommand(String cmd) {
        if (cmd.isEmpty()) {
            nText = "空命令：input 0|1 / refresh 90 / scale 1.5";
            post();
            return;
        }
        String[] t = cmd.split("\\s+");
        String sym = t[0].toLowerCase(Locale.ROOT);
        String arg = t.length > 1 ? t[1] : "";
        String res;

        switch (sym) {
            case "refresh":
                res = setNum("refresh", "refresh", arg, 24, 240);
                break;
            case "input":
                res = setInput(arg);
                break;
            case "scale":
                res = setScale(arg);
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

    private String runMkpool(String mode) {
        String r = sh(MODDIR + "/vdrm_mkpool --mode " + mode);
        return r.isEmpty() ? ("mode " + mode + " ✔") : ("mode 失败: " + r);
    }

    /** 数值参数：先校验范围，再写 sysfs + commit */
    private String setNum(String sym, String param, String arg, int lo, int hi) {
        int v;
        try {
            v = Integer.parseInt(arg);
        } catch (Exception e) {
            return sym + " 要数字";
        }
        if (v < lo || v > hi)
            return sym + " 范围 " + lo + ".." + hi;

        String r = sh("echo " + v + " > " + P + "/" + param);

        if (!r.isEmpty())
            return sym + " 失败: " + r;
        if (!commit())
            return sym + " 写入了，但 commit 失败";
        /* refresh 是开机种子之一，一起记住 */
        if (param.equals("refresh"))
            persist("refresh", String.valueOf(v));
        return sym + " " + v + " ✔";
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

    /**
     * UI 缩放：改的是**合成器**的 scale，不是 EDID 的 dpi。
     *
     * `edid_dpi` 只影响 EDID 里的物理尺寸，合成器只在输出**创建时**读它一次，
     * 之后改毫无反应 —— 所以通知里那个 dpi 看着就是"假的"。真正实时能变的
     * 是 `niri msg output <name> scale N`，这里就是转一次容器命令。
     *
     * 输出名不写死：从 `niri msg outputs` 第一行括号里取（Virtual-3 之类），
     * 换机器/换合成器不用改。
     */
    private String setScale(String arg) {
        double d;

        try {
            d = Double.parseDouble(arg);
        } catch (Exception e) {
            return "scale 要数字，例如 1.5";
        }
        if (d < 0.5 || d > 3.0)
            return "scale 范围 0.5..3.0";

        /* 找 socket 要小心：niri 的 IPC socket 名字里带 pid，niri 重启之后
         * 旧文件会留在 /tmp/xdg 里 —— 直接取第一个会连到死 socket
         * （"Invalid argument (os error 22)"）。所以按修改时间从新到旧试，
         * 用 `niri msg version` 探活，第一个活的就是它。*/
        String r = niriRun("NIRI_SOCKET=$N niri msg output \"$O\" scale " + arg + " 2>&1");

        return r.isEmpty() ? ("scale " + arg + " ✔") : ("scale 失败: " + r);
    }

    /**
     * 在容器里跑一段 niri IPC，自动挑一个**活着**的 socket。
     *
     * niri 的 socket 名字带 pid，重启之后旧文件会留在 /tmp/xdg 里 —— 直接取
     * 第一个会连到死 socket（"Invalid argument (os error 22)"）。所以按修改
     * 时间从新到旧试，用 `niri msg version` 探活。
     *
     * 传进来的 ipc 片段里可以用 $N（活 socket）和 $O（输出名，从
     * `niri msg outputs` 第一行的括号里取，Virtual-3 之类，不写死）。
     */
    private String niriRun(String ipc) {
        String inner = "N=; for S in $(ls -t /tmp/xdg/niri.wayland-*.sock 2>/dev/null); "
                + "do if NIRI_SOCKET=$S niri msg version >/dev/null 2>&1; "
                + "then N=$S; break; fi; done; "
                + "[ -z \"$N\" ] && { echo no-live-niri; exit 1; }; "
                + "O=$(NIRI_SOCKET=$N niri msg outputs 2>/dev/null | head -1"
                + " | sed -n \"s/.*(\\(.*\\)).*/\\1/p\"); "
                + "[ -z \"$O\" ] && { echo no-output; exit 1; }; "
                + ipc;

        return sh(DS + " -n " + CT + " run -- bash -lc '" + inner + "'");
    }

    /** 读当前 scale（"2.25" 这种）；读不到返回空串 */
    private String readScale() {
        String r = niriRun("NIRI_SOCKET=$N niri msg outputs 2>/dev/null"
                + " | sed -n \"s/.*Scale: *//p\" | head -1").trim();
        /* 只接受纯数字，别把报错文本塞进提示行 */
        return r.matches("[0-9]+(\\.[0-9]+)?") ? r : "";
    }

    /**
     * 把一项配置写回 `mode.conf` —— 它就是开机种子（`service.sh` 拼 insmod 参数时读）。
     *
     * 不做这件事的话，通知里改的东西一重启就没了，用户会当成 bug（"我设了全面屏，
     * 重启怎么又变回黑边了"）。写完以后："通知 = 唯一真相，mode.conf = 它的记录"。
     *
     * 只替换/追加 `key=value` 那一行，注释和别的键原样保留。
     */
    private void persist(String key, String value) {
        String f = MODDIR + "/mode.conf";
        String cmd = "F=" + f + "; "
                + "if grep -q '^" + key + "=' $F 2>/dev/null; then "
                + "  sed -i 's|^" + key + "=.*|" + key + "=" + value + "|' $F; "
                + "else echo '" + key + "=" + value + "' >> $F; fi";
        String r = sh(cmd);

        if (!r.isEmpty())
            Log.w(TAG, "persist " + key + "=" + value + " 失败: " + r);
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
