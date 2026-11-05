#!/bin/sh
# 交叉编 libvdrm_consumer.so —— 只用 NDK 的 sysroot，不用它自带的 clang。
#
# 为什么这么绕：这台机器是 aarch64 Linux，而官方 NDK 的 prebuilt clang 是
# x86_64 的（跑不了，容器里也没有 qemu/binfmt）。但 sysroot 里的头文件和
# stub 库、以及 builtins/libunwind 都是**目标架构**的，跟宿主架构无关，
# 所以拿本机 clang（aarch64）加 --target 就能编。
#
# 一次性准备（NDK r30 解出来的位置）：
#   NDK=/root/ndk/android-ndk-r30/toolchains/llvm/prebuilt/linux-x86_64
#   cp $NDK/lib/clang/21/lib/linux/libclang_rt.builtins-aarch64-android.a \
#      /usr/lib/clang/23/lib/linux/
#   mkdir -p /usr/lib/clang/23/lib/linux/aarch64
#   cp $NDK/lib/clang/21/lib/linux/aarch64/libunwind.a \
#      /usr/lib/clang/23/lib/linux/aarch64/
#
# 用法：
#   ./build-android-so.sh                 # 产物写进 ../jniLibs/arm64-v8a/
#   API=36 NDK=/path/to/ndk ./build-android-so.sh
#
# 验证不用重出 APK：把产物替换到设备上已安装的
#   /data/app/*/com.vdrm.consumer*/lib/arm64/libvdrm_consumer.so
# （chown system:system、chmod 755、restorecon），再重启 App 即可。
set -e

NDK=${NDK:-/root/ndk/android-ndk-r30/toolchains/llvm/prebuilt/linux-x86_64}
API=${API:-30}
OUT=$(cd "$(dirname "$0")" && pwd)/../jniLibs/arm64-v8a/libvdrm_consumer.so

test -d "$NDK/sysroot" || { echo "找不到 NDK sysroot: $NDK/sysroot" >&2; exit 1; }

cd "$(dirname "$0")"
clang --target=aarch64-linux-android"$API" \
      --sysroot="$NDK/sysroot" \
      -fuse-ld=lld -O2 -fPIC -shared -I. \
      -o "$OUT" \
      native_consumer.c ahb_rebuild.c \
      -llog -landroid -laaudio -lEGL -lGLESv2 -ldl

echo "→ $OUT"
readelf -d "$OUT" | grep NEEDED
file "$OUT"
