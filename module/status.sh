#!/system/bin/sh
# VDRM 模块状态 —— 三段信息写进 module.prop 的 description，管理器直接显示，不用点。
#
# 这套是照 Droidspaces 的模块抄的。它的 service.sh 里就一句：
#     sed -i "s/^description=.*/description=$*/g" "$MODDIR/module.prop"
# 也就是说：状态【就是写在 module.prop 里的】，管理器读那一行显示出来 ——
# 所以看起来"自己就写着"，不需要谁去执行什么。
#
# 两个讲究：
#   - 分三段用 " | " 隔开，太长管理器那边自然折行
#   - 状态用 emoji 表色（🟢 好 / 🔴 坏 / 🟡 未知）—— module.prop 是纯文本，
#     ANSI 颜色塞不进去，Droidspaces 也是这么干的
#
#   status.sh           只更新 module.prop（boot 时由 service.sh 调）
#   status.sh --print   把同样的三段一行一段打印出来（手动看 / 排错时用）

MODDIR=${0%/*}
PRINT=0
[ "$1" = "--print" ] && PRINT=1

# ── 第一段：虚拟键鼠对 Android 藏好没有 ────────────────────────────────────
# 判据是【真状态】，不是配置文件：
#   - 三台 vdrm-* 的 /dev/input/eventN 必须是我们自己 mknod 的 0600 root:root
#     （0600 才是"Android 打不开"的关键；ueventd 建的那份是 0660 root:input）
# 节点由 post-fs-data.sh / service.sh 在加载后一次性删掉重建；不再需要常驻进程。
VKM=0
for f in /sys/class/input/event*/device/name; do
    [ -e "$f" ] || continue
    case "$(cat "$f" 2>/dev/null)" in
        vdrm-*) ;;
        *) continue ;;
    esac
    node="/dev/input/$(basename "${f%/device/name}")"
    [ -c "$node" ] || continue
    [ "$(stat -c '%a:%U' "$node" 2>/dev/null)" = "600:root" ] && VKM=$((VKM + 1))
done
if [ "$VKM" -ge 3 ]; then
    S1="Virtual keyboard & mouse: 🟢"
else
    S1="Virtual keyboard & mouse: 🔴 Android can still see it — run: sh $MODDIR/service.sh"
fi

# ── 第二段：内核信息 ───────────────────────────────────────────────────────
# 就是 service.sh 选 .ko 的那套算法：从 uname -r 拼出 KMI。
# GKI 的版本串同时带着版本号和 android 大版本，缺一个都定不了档：
#
#     6.1.141-android14-11-o-gca13bffobf09
#     └─6.1─┘  └─android14─┘
REL=$(uname -r)
VER=$(echo "$REL" | cut -d. -f1,2)
AND=$(echo "$REL" | sed -n 's/.*\(android[0-9][0-9]*\).*/\1/p')
KMI="$AND-$VER"
S2="Kernel: $KMI"

# ── 第三段：VDRM 就绪没有 ──────────────────────────────────────────────────
if lsmod 2>/dev/null | grep -q '^vdrm ' && [ -e /dev/vdrm ]; then
    S3="VDRM: ready 😋"
else
    S3="VDRM: unavailable 🤔 — vdrm-$KMI.ko not loaded"
fi

# ── 写回 module.prop（管理器就是读这一行）──────────────────────────────────
#
# 这里【不能用 sed】。Droidspaces 用的是
#     sed -i "s/^description=.*/description=$*/g" module.prop
# 它那句侥幸没事，但我们的第一段叫 "Virtual keyboard & mouse" —— 替换串里的 & 在
# sed 里代表【整个匹配到的内容】，会被展开成旧的整行，把这一行搅成一团。
# 用 awk 整行替换：没有 & 这个坑，位置也不动。
PROP=$MODDIR/module.prop
if [ -f "$PROP" ]; then
    awk -v d="description=$S1 | $S2 | $S3" \
        '/^description=/ { print d; next } { print }' "$PROP" > "$PROP.tmp" \
        && cat "$PROP.tmp" > "$PROP"
    rm -f "$PROP.tmp"
fi

# ── 手动打印：同样的三段，一行一段 ─────────────────────────────────────────
# 长相跟 module.prop 那行刻意保持一致：emoji 表状态，不用 ANSI 颜色。
# 模块里【没有 action.sh】（v3.14 起删掉了：配置入口统一交给 App 的通知，
# 不在管理器里再多挂一个"执行"按钮）。要看状态就 `sh status.sh --print`。
if [ "$PRINT" = 1 ]; then
    printf '%s\n%s\n%s\n' "$S1" "$S2" "$S3"
fi
