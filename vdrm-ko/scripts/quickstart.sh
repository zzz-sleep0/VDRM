#!/data/data/com.termux/files/usr/bin/sh
#
# VDRM 内核模块 · 速通脚本
#
# 目标：从零到一个你这个内核能用的 vdrm.ko。全程在 Termux 里跑。
# 需要 root 的两步（设备快照 / CRC 采集）脚本自己调 su，不用手动切 shell。
#
# 用法：
#   sh scripts/quickstart.sh               # 构建
#   sh scripts/quickstart.sh --load        # 构建 + 直接加载
#   VDRM_REF_KO=/path/xxx.ko sh scripts/quickstart.sh
#   VDRM_WORK=$HOME/other   sh scripts/quickstart.sh
#
# 只做本机（同设备）。换内核/换设备要重跑；每一步的产物都留在磁盘上，可以单独重来。

set -eu

KIT=$(cd "$(dirname "$0")/.." && pwd)
WORK="${VDRM_WORK:-$HOME/vdrm-port}"
KP="${KPATCH_BIN:-/data/adb/modules/KPatch-Next/bin/kpatch}"
KP_SRC="$KIT/KPatch-Next-main"

say()  { printf '\n\033[1m[%s]\033[0m %s\n' "$1" "$2"; }
ok()   { printf '    \033[32m✓\033[0m %s\n' "$*"; }
die()  { printf '\n\033[31m[中断]\033[0m %s\n' "$*" >&2; exit 1; }

# ---------------------------------------------------------------- 0 环境
say 0/6 "环境检查"
[ -n "${PREFIX:-}" ] || die "不是 Termux 环境（\$PREFIX 为空）。本流程设计为在 Termux 里跑。"

if ! su -c id 2>/dev/null | grep -q 'uid=0'; then
    die "su 不可用或没有 root。本流程需要 root（读 BTF / 采集 CRC / 加载模块）。"
fi
ok "root 可用"

OBJCOPY=objcopy
MISS=""
for t in clang ld.lld python3 nm readelf modprobe modinfo pahole; do
    command -v "$t" >/dev/null 2>&1 || MISS="$MISS $t"
done
command -v objcopy >/dev/null 2>&1 || { command -v gobjcopy >/dev/null 2>&1 && OBJCOPY=gobjcopy || MISS="$MISS objcopy"; }

if [ -n "$MISS" ]; then
    say 0/6 "缺工具，正在安装：$MISS"
    pkg install -y clang lld python binutils dwarves kmod
    for t in clang ld.lld python3 nm readelf modprobe modinfo pahole; do
        command -v "$t" >/dev/null 2>&1 || die "安装后仍缺 $t，请手工解决依赖后重跑"
    done
fi
ok "编译工具齐（objcopy=$OBJCOPY）"

[ -x "$KP" ] || die "找不到 KPatch-Next：$KP
    它只用来读内核 CRC 和类型哈希，最终产物是普通 .ko。
    装好后或用 KPATCH_BIN=/你的路径/kpatch 重跑。"

if [ ! -d "$KP_SRC" ]; then
    say 0/6 "下载 KPatch-Next 源码（仅需要它的内核头）"
    curl -fL 'https://gh-proxy.org/https://github.com/KernelSU-Next/KPatch-Next/archive/refs/heads/main.tar.gz' \
         -o "$KIT/KPatch-Next.tar.gz" || die "下载失败（网络？代理？）"
    tar -xzf "$KIT/KPatch-Next.tar.gz" -C "$KIT"
fi
ok "KPatch-Next 头文件就绪"

mkdir -p "$WORK"

# 需要 root 的两步放进一个 helper，避免多层引号
cat > "$WORK/rootstep.sh" <<'ROOTEOF'
#!/system/bin/sh
# 由 quickstart.sh 生成，只在 root shell 里跑
set -eu
cmd=$1; KIT=$2; WORK=$3; shift 3

find_ref_ko() {
    for d in /vendor/lib/modules /vendor_dlkm/lib/modules /system/lib/modules /odm/lib/modules; do
        [ -d "$d" ] || continue
        for f in "$d"/*.ko; do
            [ -f "$f" ] || continue
            strings "$f" 2>/dev/null | grep -q '^vermagic=' || continue
            if strings "$f" 2>/dev/null | grep -qx 'module_layout' && \
               strings "$f" 2>/dev/null | grep -qx 'fget' && \
               strings "$f" 2>/dev/null | grep -qx 'mutex_lock'; then
                echo "$f"; return 0
            fi
        done
    done
    return 1
}

case "$cmd" in
snapshot)
    target=$1; ref=$2
    mkdir -p "$target"
    cp /sys/kernel/btf/vmlinux "$target/vmlinux.btf"
    gzip -dc /proc/config.gz > "$target/config.txt"
    uname -r > "$target/kernel-release.txt"
    strings "$ref" | grep '^vermagic=' | cut -d= -f2- > "$target/vermagic.txt"
    [ "$(wc -l < "$target/vermagic.txt")" -eq 1 ] || { echo "REF-BAD"; exit 1; }
    cp "$ref" "$target/anchor.ko"
    echo "SNAPSHOT-OK"
    ;;
collect)
    build=$1; out=$2
    mkdir -p /data/local/tmp/vdrm-port-session "$out"
    cp "$build/collector.kpm" /data/local/tmp/vdrm-port-session/collector.kpm
    uname -r > "$out/kernel-release.txt"
    if "$KP_BIN" kpm load /data/local/tmp/vdrm-port-session/collector.kpm; then
        "$KP_BIN" kpm unload vdrm_port_crc || true
    else
        echo "COLLECT-LOAD-FAIL"; exit 1
    fi
    cp /data/local/tmp/vdrm-port-session/crcs.tsv "$out/crcs.tsv"
    [ -f /data/local/tmp/vdrm-port-session/hashes.tsv ] && \
        cp /data/local/tmp/vdrm-port-session/hashes.tsv "$out/hashes.tsv"
    echo "COLLECT-OK"
    ;;
findref)
    find_ref_ko
    ;;
insmod)
    ko=$1
    grep -q '^vdrm ' /proc/modules && { rmmod vdrm || { echo "RMMOD-FAIL"; exit 1; }; }
    insmod "$ko"
    echo "INSMOD-OK"
    ;;
esac
ROOTEOF
chmod 755 "$WORK/rootstep.sh"

KP_BIN="$KP"
export KP_BIN

# 新目录助手：已存在就换后缀，绝不覆盖
fresh() {
    base=$1; d=$base
    i=2
    while [ -e "$d" ]; do d="$base-$i"; i=$((i+1)); done
    echo "$d"
}

# ---------------------------------------------------------------- 1 快照
say 1/6 "读设备信息"
REF="${VDRM_REF_KO:-}"
if [ -z "$REF" ]; then
    REF=$(su -c "sh $WORK/rootstep.sh findref" 2>/dev/null | tail -1 || true)
    [ -n "$REF" ] || die "自动找不到合适的参考模块（要同时引用 module_layout / fget / mutex_lock 的未压缩 .ko）。
    用 VDRM_REF_KO=/你的/xxx.ko 指定一个。"
    ok "自动选中参考模块：$REF"
else
    ok "使用指定的参考模块：$REF"
fi

TARGET=$(fresh "$WORK/target")
su -c "sh $WORK/rootstep.sh snapshot '$TARGET' '$REF'" | grep -q SNAPSHOT-OK \
    || die "快照失败。检查参考模块是否未压缩、是否属于本设备。"
ok "target → $TARGET"
ok "内核 $(cat "$TARGET/kernel-release.txt")"

# ---------------------------------------------------------------- 2 prepare
say 2/6 "生成类型头 + 编译驱动对象"
BUILD=$(fresh "$WORK/build")
python3 "$KIT/scripts/port.py" prepare \
    --kp "$KP_SRC" \
    --target "$TARGET" \
    --build "$BUILD" \
    --objcopy "$OBJCOPY" || die "prepare 失败。**不要**继续 collect，先按提示修。"
ok "build → $BUILD"

# ---------------------------------------------------------------- 3 采集
say 3/6 "采集 CRC / kCFI"
CAPTURE=$(fresh "$WORK/capture")
su -c "KP_BIN='$KP' sh $WORK/rootstep.sh collect '$BUILD' '$CAPTURE'" | grep -q COLLECT-OK \
    || die "采集失败。看 dmesg；/data/local/tmp/vdrm-port-session/ 里可能有半成品。"
ok "capture → $CAPTURE（$(grep -c . "$CAPTURE/crcs.tsv") 条 CRC）"

# ---------------------------------------------------------------- 4 finish
say 4/6 "校验并链接模块"
python3 "$KIT/scripts/anchors.py" "$TARGET/anchor.ko" "$CAPTURE/anchors.tsv" \
    || die "anchors 提取失败（参考模块要引用 module_layout/fget/mutex_lock）"
python3 "$KIT/scripts/port.py" finish --build "$BUILD" --capture "$CAPTURE" \
    || die "finish 校验没过。**不要**改 CRC 数值或关 kCFI 去绕。"
KO="$BUILD/vdrm.ko"
[ -f "$KO" ] || die "没有产出 $KO"
ok "$KO"

# ---------------------------------------------------------------- 5 就位
say 5/6 "放到设备上"
su -c "cp '$KO' /data/local/tmp/vdrm.ko && chmod 644 /data/local/tmp/vdrm.ko"
ok "/data/local/tmp/vdrm.ko"

# ---------------------------------------------------------------- 6 加载
if [ "${1:-}" = "--load" ]; then
    say 6/6 "加载"
    su -c "sh $WORK/rootstep.sh insmod /data/local/tmp/vdrm.ko" | grep -q INSMOD-OK \
        || die "加载失败。**不要**用 insmod -f。先看 dmesg | grep -i vdrm"
    su -c 'dmesg | grep -i "vdrm: bring-up"' || true
    ok "已加载"
else
    say 6/6 "加载（未执行，你自己决定）"
    cat <<'EOM'
    先停掉用 vdrm 的 App 和合成器，然后：

        su -c 'rmmod vdrm 2>/dev/null; insmod /data/local/tmp/vdrm.ko'
        su -c 'dmesg | grep -i vdrm'

    或直接重跑本脚本并加 --load。

    加载成功后照常起 App、注册缓冲池、起合成器。
    输入设备（vdrm-kbd / vdrm-ptr）还要求容器侧开「完整硬件访问」+
    特权模式 nomask，并按节点绑定 —— 见 README。
EOM
fi

say "完成" "产物：$KO"
