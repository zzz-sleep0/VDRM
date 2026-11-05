#!/bin/sh
# 容器侧原生验收：节点是否绑好 + DRM 配对 + EGL/GPU + KMS 翻转链路
set -u
cd "$(dirname "$0")" || exit 1
fail=0
echo "== nodes =="
for n in /dev/dri/card1 /dev/dri/renderD129 /dev/dri/renderD128 /dev/kgsl-3d0 /dev/vdrm /dev/vdrm-audio /dev/dma_heap/system; do
	if [ -e "$n" ]; then
		printf '  OK   %-28s ' "$n"
		ls -l "$n" | awk '{print $1, $5, $6}'
	else
		printf '  MISS %s\n' "$n"
		fail=1
	fi
done

echo "== DRM 配对 =="
./drmpair || fail=1

echo "== EGL（card1，真 GPU）=="
./eglprobe5 /dev/dri/card1 2>&1 | grep -vE '^TU:|^libGL' || fail=1

echo "== KMS 翻转链路 =="
./kms_flip_test || fail=1

echo
[ "$fail" = 0 ] && echo "== ALL OK ==" || echo "== 有失败项 =="
exit "$fail"
