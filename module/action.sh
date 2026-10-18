#!/system/bin/sh
# VDRM 分辨率选择 —— 模块 Action 按钮触发。
#
# 5 秒内：
#   音量上 = 按真实尺寸采最大 16:9（居中，两侧留黑边，避开摄像头挖空）
#   音量下 = 当前真实横屏分辨率（铺满，摄像头挖空会压在桌面上）
#   不按   = 保持当前配置
#
# 两个选择都来自“采集”：音量上是采真实尺寸后算出的安全 16:9，音量下是真实尺寸本身。
# 1920x1080 只是 .ko 自带的兜底默认，不是这里写死的选项。
#
# 结果写入 mode.conf，下次启动由 service.sh 取用。

MODDIR=${0%/*}
CONF="$MODDIR/mode.conf"
TMPK=/data/local/tmp/vdrm_action_keys

say() {
    echo "$1"
    cmd notification post -S bigtext -t "VDRM 分辨率" vdrm_mode "$1" >/dev/null 2>&1
}

CUR=$(sed -n 's/^mode=//p' "$CONF" 2>/dev/null | tr -d ' \r' | tail -n1)
[ -z "$CUR" ] && CUR=1920x1080

# 真实分辨率：优先 Override（PC 模式），竖屏尺寸转横屏。
REAL=$(wm size 2>/dev/null | sed -n 's/.*Override size: *//p' | head -n1)
[ -z "$REAL" ] && REAL=$(wm size 2>/dev/null | sed -n 's/.*Physical size: *//p' | head -n1)
if [ -n "$REAL" ]; then
    W=${REAL%x*}
    H=${REAL#*x}
    case "$W$H" in *[!0-9]*) REAL= ;; esac
    if [ -n "$REAL" ] && [ "$W" -lt "$H" ]; then
        REAL="${H}x${W}"
    fi
fi
[ -z "$REAL" ] && REAL=2376x1080

# 音量上：以真实尺寸为基准取最大 16:9（居中，两侧黑边，避开挖空）。
# 真实宽高变了这里跟着变 —— 1920x1080 只是 .ko 的兜底默认，不是写死的。
W=${REAL%x*}
H=${REAL#*x}
SAFE_H=$H
SAFE_W=$((SAFE_H * 16 / 9))
if [ "$SAFE_W" -gt "$W" ]; then
    SAFE_W=$W
    SAFE_H=$((SAFE_W * 9 / 16))
fi
SAFE="${SAFE_W}x${SAFE_H}"

rm -f "$TMPK"
(timeout 5 getevent -l -q > "$TMPK" 2>/dev/null) &
GPID=$!

say "5 秒内选择：音量上=$SAFE 留黑边；音量下=$REAL 铺满（摄像头挖空）；不按=保持 $CUR"

CHOICE=
for i in 5 4 3 2 1; do
    sleep 1
    if grep -q "KEY_VOLUMEUP" "$TMPK" 2>/dev/null; then
        CHOICE="$SAFE"
        break
    fi
    if grep -q "KEY_VOLUMEDOWN" "$TMPK" 2>/dev/null; then
        CHOICE="$REAL"
        break
    fi
    say "还有 $i 秒：音量上=$SAFE，音量下=$REAL，不按=保持"
done
wait $GPID 2>/dev/null

if [ -n "$CHOICE" ]; then
    {
        echo "# VDRM 输出分辨率（1920x1080 / 2376x1080 / auto）"
        echo "mode=$CHOICE"
    } > "$CONF"
    say "已选择 $CHOICE，重启后生效"
else
    say "未选择，保持 $CUR"
fi
