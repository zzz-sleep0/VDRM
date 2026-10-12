#!/system/bin/sh
# Invoke in Android root shell: sh host.sh snapshot DIR REFERENCE_MODULE
# Then: sh host.sh collect BUILD_DIR CAPTURE_DIR
set -eu
[ "$(id -u)" = 0 ] || { printf 'Enter Android root shell with su first.\n'; exit 1; }
case "${1:-}" in
snapshot)
    out=$2
    ref=$3
    [ ! -e "$out" ] || { printf 'Choose a new snapshot directory.\n'; exit 1; }
    mkdir -p "$out"
    cp /sys/kernel/btf/vmlinux "$out/vmlinux.btf"
    gzip -dc /proc/config.gz > "$out/config.txt"
    uname -r > "$out/kernel-release.txt"
    # Android may not ship modinfo. Extract the NUL-terminated modinfo string.
    strings "$ref" | grep '^vermagic=' | cut -d= -f2- > "$out/vermagic.txt"
    [ "$(wc -l < "$out/vermagic.txt")" -eq 1 ] || { printf 'Cannot identify vermagic.\n'; exit 1; }
    cp "$ref" "$out/anchor.ko"
    printf 'Snapshot: %s\n' "$out"
    ;;
collect)
    build=$2
    out=$3
    kp=${KPATCH_BIN:-/data/adb/modules/KPatch-Next/bin/kpatch}
    work=/data/local/tmp/vdrm-port-session
    [ ! -e "$out" ] && [ ! -e "$work" ] || { printf 'Preserve/rename old output and /data/local/tmp/vdrm-port-session before rerunning.\n'; exit 1; }
    mkdir -p "$work" "$out"
    cp "$build/collector.kpm" "$work/collector.kpm"
    uname -r > "$out/kernel-release.txt"
    if "$kp" kpm load "$work/collector.kpm"; then
        "$kp" kpm unload vdrm_port_crc
    else
        printf 'Collector failed; inspect dmesg and partial files in %s. Do not force load.\n' "$work"
        exit 1
    fi
    cp "$work/crcs.tsv" "$out/crcs.tsv"
    if [ -f "$work/hashes.tsv" ]; then cp "$work/hashes.tsv" "$out/hashes.tsv"; fi
    printf 'Capture: %s. Collector unloaded; vdrm was not touched.\n' "$out"
    ;;
*) printf 'Usage: sh host.sh snapshot NEW_DIR REFERENCE_KO | collect BUILD_DIR NEW_CAPTURE_DIR\n'; exit 1;;
esac
