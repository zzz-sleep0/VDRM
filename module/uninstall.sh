#!/system/bin/sh
# 卸载：把自己留下的东西【全收掉】，不指望管理器帮忙。
#
# 管理器（KernelSU / Magisk）点"移除"时的顺序是：先跑本脚本、再删模块目录 ——
# 所以按契约，目录其实不用自己动。但自己动更保险：手工删目录、或者管理器哪天
# 换了行为，这里一样是干净的。

ID=vdrm-lkm

# 1) 内核里那个模块。合成器或 App 正占着 DRM 设备就会失败 —— 那就等重启，重启后
#    它自然不会再被加载。尽力而为，不阻断后面的清理。
rmmod vdrm 2>/dev/null

# 2) 早期版本把建池 helper 和它的日志拷到 /data/wayland（当时那个路径还叫这名）。
#    它已退役：helper 改成"就地跑、日志写自己旁边"，这两份是没人用的残留。
rm -f /data/wayland/vdrm_mkpool /data/wayland/mkpool.log

# 3) adbd 保活守护：先停进程，再收掉它的开机钩子副本。
if [ -f /data/local/tmp/adbd_keep.pid ]; then
    kill "$(cat /data/local/tmp/adbd_keep.pid)" 2>/dev/null
    rm -f /data/local/tmp/adbd_keep.pid
fi
rm -f /data/adb/service.d/adbd_keep.sh /data/adb/service.d/.adbd_keep.sh.new
rm -f /data/local/tmp/adbd_keep.log

# 4) 模块目录自己，外加可能存在的更新暂存目录。
#    —— 放在【最后】一行，因为这一步会把自己也删掉：shell 已把本文件读进内存了，
#    删了不影响正在跑的这几步，但放最后更稳妥。
rm -rf "/data/adb/modules/$ID" "/data/adb/modules_update/$ID"
