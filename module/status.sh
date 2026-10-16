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

# ── 第一段：虚拟键鼠有没有被排除 ───────────────────────────────────────────
# 判据是【覆盖有没有真的落上】：生效时 /vendor/etc/excluded-input-devices.xml
# 就是模块里那份（metamodule 用 overlayfs 把它从 userdata 挂过去），内容必然
# 逐字节一致；没生效时那里是 ROM 的原版，或者压根不存在。
# 不能靠看挂载表判断 —— 生效时 mount 显示的是底层块设备，不是模块路径。
XML_MOD=$MODDIR/system/vendor/etc/excluded-input-devices.xml
XML_SYS=/vendor/etc/excluded-input-devices.xml
[ -f "$XML_SYS" ] || XML_SYS=/system/vendor/etc/excluded-input-devices.xml

live=$(md5sum "$XML_SYS" 2>/dev/null | cut -d' ' -f1)
want=$(md5sum "$XML_MOD" 2>/dev/null | cut -d' ' -f1)

if [ -n "$live" ] && [ "$live" = "$want" ]; then
    S1="Virtual keyboard & mouse: 🟢"
else
    S1="Virtual keyboard & mouse: 🔴 It won't work correctly — install a metamodule to solve it"
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
# 模块里【没有 action.sh】—— 有的话管理器会多挂一个"执行"按钮，Droidspaces
# 就没有，所以这边也不放。要看状态就 `sh status.sh --print`。
if [ "$PRINT" = 1 ]; then
    printf '%s\n%s\n%s\n' "$S1" "$S2" "$S3"
fi
