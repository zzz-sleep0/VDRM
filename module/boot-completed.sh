#!/system/bin/sh
# 开机完成后把 /dev/vdrm 交给 VDRM App。
# service.sh 执行时 package manager 可能还没就绪，UID 查不到就会留成 root:root 600，
# App 打不开设备 → 黑屏。这里在 boot completed 后重试，直到拿到 UID。
MODDIR=${0%/*}
APP_UID=
i=0
while [ $i -lt 30 ]; do
    APP_UID=$(cmd package list packages -U 2>/dev/null \
              | sed -n 's/.*package:com\.vdrm\.consumer uid:\([0-9][0-9]*\).*/\1/p' \
              | head -n1)
    [ -z "$APP_UID" ] && APP_UID=$(stat -c '%u' /data/user/0/com.vdrm.consumer 2>/dev/null)
    [ -z "$APP_UID" ] && APP_UID=$(stat -c '%u' /data/data/com.vdrm.consumer 2>/dev/null)
    [ -n "$APP_UID" ] && break
    i=$((i + 1))
    sleep 1
done

if [ -n "$APP_UID" ] && [ -e /dev/vdrm ]; then
    chown "$APP_UID:$APP_UID" /dev/vdrm
    chmod 600 /dev/vdrm
    chcon u:object_r:gpu_device:s0 /dev/vdrm 2>/dev/null
    log -t vdrm "boot-completed: /dev/vdrm owner=$APP_UID"
else
    log -t vdrm "boot-completed: vdrm/app uid not ready"
fi
