# DDK 环境 —— 照抄 Ylarod/ddk-module-template，改用 android14-6.1
#
# 三种用法：
#   Host 模式       DDK 解压在 /opt/ddk，直接 source 本文件后 make
#   DevContainer    .devcontainer/devcontainer.json 里已经设好，进容器直接 make
#   Docker（CI）    .github/workflows/build.yml 用 ddk-min 镜像，见那里
#
# KMI 与 clang 的对应关系取自 DDK 的 mapping.json：
#   android12-5.10 -> clang-r416183b    android15-6.6 -> clang-r510928
#   android13-5.10 -> clang-r450784e    android16-6.12 -> clang-r536225
#   android14-5.15 -> clang-r487747c    android17-6.18 -> clang-r584948c
#   android14-6.1  -> clang-r487747c    ← 本机设备就是这一档

export DDK_ROOT=/opt/ddk

export KDIR=$DDK_ROOT/kdir/android14-6.1
export CLANG_PATH=$DDK_ROOT/clang/clang-r487747c/bin

export PATH=$CLANG_PATH:$PATH
export CROSS_COMPILE=aarch64-linux-gnu-
export ARCH=arm64
export LLVM=1
export LLVM_IAS=1

gmake
