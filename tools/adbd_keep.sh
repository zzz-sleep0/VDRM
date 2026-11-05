#!/system/bin/sh
# VDRM —— adbd 保活守护
#
# 目标：adbd 一直活着，并且一直听着 TCP 5555。
#
# 老版本是每 20 秒无条件 stop+start 一次，两个毛病：
#   1. 每 20 秒把正在用的 adb 连接掐断一次 —— 这边每条 adb 命令都有概率
#      撞上 "device not found"/"Connection refused"；
#   2. 守护万一死在 stop 和 start 之间，adbd 就永久停在"停"上，再没人拉起
#      它（实测踩过一次，adb 彻底连不上，只能手上去 start adbd）。
#
# 现在改成"有病才治"：定期看 init.svc.adbd / service.adb.tcp.port / 5555
# 有没有监听，只有不正常才 setprop + stop + start，并且 start 之后要校验，
# 校验不过就重试。守护刚起来时也会先自检一遍，保证"守护在、adbd 却是停的"
# 这种状态立刻被修正。

INTERVAL=${VDRM_ADBD_INTERVAL:-5}
LOGF=/data/local/tmp/adbd_keep.log
PIDF=/data/local/tmp/adbd_keep.pid
PORT=5555

log() { echo "$(date '+%m-%d %H:%M:%S') $*" >> "$LOGF"; }

alive() {
    [ -n "$1" ] && [ -d "/proc/$1" ] || return 1
    tr '\0' ' ' < "/proc/$1/cmdline" 2>/dev/null | grep -q adbd_keep
}

has_netstat() { command -v netstat >/dev/null 2>&1; }

port_listening() {
    has_netstat || return 0        # 没有 netstat 就只看服务状态
    netstat -ltn 2>/dev/null | grep -q ":$PORT "
}

healthy() {
    [ "$(getprop init.svc.adbd)" = "running" ] || return 1
    [ "$(getprop service.adb.tcp.port)" = "$PORT" ] || return 1
    port_listening
}

# 修一次：设属性 → 重启 adbd → 等它真的跑起来并监听端口
fix_adbd() {
    local why=$1
    setprop service.adb.tcp.port "$PORT"
    stop adbd
    start adbd
    local i=0
    while [ "$i" -lt 10 ]; do
        sleep 1
        if healthy; then
            log "fixed ($why): svc=$(getprop init.svc.adbd) port=$PORT"
            return 0
        fi
        i=$((i + 1))
    done
    log "FIX FAILED ($why): svc=$(getprop init.svc.adbd) prop=$(getprop service.adb.tcp.port)"
    # 起不来就再来一次 setprop + start（有时 stop 还没落定就 start 会失败）
    setprop service.adb.tcp.port "$PORT"
    start adbd
    return 1
}

# 单实例
if [ -f "$PIDF" ] && alive "$(cat "$PIDF" 2>/dev/null)"; then
    exit 0
fi
echo $$ > "$PIDF"

if [ -f "$LOGF" ] && [ "$(wc -c < "$LOGF")" -gt 262144 ]; then
    : > "$LOGF"
fi

log "start pid=$$ interval=${INTERVAL}s"
sleep 1

while true; do
    healthy || fix_adbd "not healthy"
    sleep "$INTERVAL"
done
