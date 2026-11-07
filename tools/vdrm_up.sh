#!/system/bin/sh
# VDRM 一键起环境：模块 -> 节点 -> 池 -> App -> niri，跑完就能直接测。
#
# 用法（手机 root shell 里）：
#   sh /data/local/tmp/vdrm_up.sh            # 模块已在就直接起环境
#   sh /data/local/tmp/vdrm_up.sh --reload   # 重载模块后再起环境（换了 .ko 用这个）
#   KO=/data/local/tmp/vdrm-new.ko sh ... --reload   # 指定要加载的 .ko
#
# 顺序不是随便排的：
#   1) App 必须先起来 —— 它会把手机屏幕尺寸上报给内核，modes 列表里才会出现
#      1264x2780 / 2780x1264；App 没起时列表只有 1080x1920 / 1920x1080，
#      这时怎么设 mode 参数都切不到竖屏全屏档。
#   2) 池要按"当前生效尺寸"建（mode_slots 反映的就是这个），所以 mkpool 放在
#      App 起来之后。
#   3) niri 用 setsid 起在独立会话里：droidspaces run 的会话一断（adb 掉线、
#      命令超时）它就会收到 SIGHUP 被杀，之前踩过好几次。
set -u

MOD=/data/adb/modules/vdrm-lkm
DS=/data/local/Droidspaces/bin/droidspaces
CT=Arch-dev
# 空 = 让 common.sh 的 vdrm_load 按 uname -r 拼出 KMI 再找 .ko；
# 设了就用这个（换新 .ko 时：KO=/data/local/tmp/vdrm-new.ko）。
KO=${KO:-}
RELOAD=0
[ "${1:-}" = "--reload" ] && RELOAD=1

say() { echo "[vdrm-up] $*"; }

# 按命令行杀的：pkill -f 会连自己这条命令一起匹配掉（踩过）
kill_by_args() {
    for p in $(ps -A -o pid,args | grep -E "$1" | grep -v grep | awk '{print $1}'); do
        kill "$p" 2>/dev/null
    done
}

# ---- 0. 需要一个干净的引用：App / niri / 声音都放手 ----
if [ "$RELOAD" = 1 ]; then
    say "重载模块：先放掉所有占用"
    am force-stop com.vdrm.consumer >/dev/null 2>&1
    $DS -n "$CT" run -- bash -lc 'pkill -x niri; pkill -x pulseaudio' >/dev/null 2>&1
    sleep 2
    rmmod vdrm 2>/dev/null
    if grep -q '^vdrm ' /proc/modules; then
        say "rmmod 失败，还有进程占着 /dev/vdrm："
        fuser -v /dev/vdrm 2>/dev/null | head -5
        exit 1
    fi
fi

# ---- 1. 模块：走 common.sh 的 vdrm_load ----
# 不在这里直接 insmod：那样会绕开 mode.conf（mode / dpi / refresh 三个开机种子
# 都由 vdrm_load 拼成 insmod 参数）。重载和新装必须同一条路径，否则
# `vdrm_up.sh --reload` 之后分辨率会悄悄回到 .ko 默认值。
MODDIR=$MOD          # common.sh 用 MODDIR 找 mode.conf / vdrm-$KMI.ko
. "$MOD/common.sh"
vdrm_load || { say "模块加载失败（见 dmesg / logcat -s vdrm）"; exit 1; }
say "模块就绪 $KO"

# ---- 2. 节点：以 sysfs 为准校正主次号/权限/标签 ----
vdrm_sync_nodes
say "设备节点已按 sysfs 校正"

# ---- 3. App 先起来（上报屏幕尺寸，modes 列表才完整）----
am start -n com.vdrm.consumer/.MainActivity >/dev/null 2>&1
sleep 3
MODES=$(cat /sys/module/vdrm/parameters/modes 2>/dev/null)
say "modes=$MODES"

# ---- 4. 池：按当前生效尺寸重建 ----
"$MOD/vdrm_mkpool" --reset >/dev/null 2>&1
say "active=$(cat /sys/module/vdrm/parameters/mode) slots=$(cat /sys/module/vdrm/parameters/mode_slots)"

# ---- 5. niri ----
$DS -n "$CT" run -- sh -c 'test -f /etc/udev/rules.d/99-vdrm-only-input.rules' >/dev/null 2>&1 ||
    say "警告：容器里缺 99-vdrm-only-input.rules，niri 会去多开手机真实输入设备"

for p in $(ps -A -o pid,args | grep -E ' niri$' | awk '{print $1}'); do
    kill -9 "$p" 2>/dev/null
done
sleep 1
$DS -n "$CT" run -- bash -lc \
    'setsid nohup niri > /tmp/niri.log 2>&1 < /dev/null &' >/dev/null 2>&1
sleep 4

MODE=$($DS -n "$CT" run -- bash -lc \
       "grep -oE 'picking mode: Mode \{ name: \"[0-9]+x[0-9]+\"' /tmp/niri.log | tail -1" 2>/dev/null)
say "niri $MODE"
say "frames_pushed=$(cat /sys/module/vdrm/parameters/frames_pushed) reuse=$(cat /sys/module/vdrm/parameters/frames_reuse)"
say "可以测了（App 在前台时画面才会被消费）"
