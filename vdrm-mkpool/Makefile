# vdrm_mkpool —— 用户态"造池"程序
#
# 用 gralloc 分配 N 块模式尺寸的 AHB（真货，显示管线认），每块取出
# {fd0(像素), fd1(metadata blob), ints} 再 REGISTER 进内核侧的池。
# 池本身由内核持有（fget 住那两个 fd），所以本进程跑完就退没关系。
#
# 编译用 NDK 的 bionic clang。Termux 里就是 $PREFIX/bin/aarch64-linux-android-clang：
#
#   make CC=/data/data/com.termux/files/usr/bin/aarch64-linux-android-clang
#
# 两点别动：
#   - 不加 -O2。加了产物就变，而线上部署的是不加的那份。
#   - 源码路径会经 __FILE__（assert 之类）编进二进制，所以"重编比对哈希"时必须
#     在【同名路径】下编，否则 md5 必然不同。这条对 App 的 .so 同样成立。
#
# 自校验（在某个目录下 `cd` 进去再编，源码叫 mkpool.c）：
#   md5sum vdrm_mkpool  →  fbfbf29caf86ff76abb7e1ecb62298fe   (16040 B)
#
# 这一版加了 `--pool WxH`：只补这个尺寸的槽，不写 mode、不 commit。
# 内核在合成器挑了一档、那档还没槽时把尺寸挂到 want_mode，App 看到就跑它
# （`want pool 2780x1264` → mkpool --pool → REGISTER → dumb_create 醒来）。
# 上一版是 c7b653f9…（15528 B）—— 只有 `--mode WxH`。
# 再上一版是 f99801ad…（13896 B）—— `--mode WxH` 一条命令走完
# prepare（写 mode）→ 建槽 → commit（EDID + hotplug）。
# 再上一版是 5c62aecd…（13280 B）—— 建池前自己用 `wm size`
# 查面板尺寸并 SET_SCREEN（不再依赖 App 上报窗口尺寸）。
# 更早是 27b5fd1a…（11680 B）
# 更早是 8f7cf89b…（10144 B）—— 分配档位补上
# GPU_COLOR_OUTPUT（对齐 App/anland 的分配质量），并在 mkpool.log 里
# 打印 describe 与原始 handle 元数据，方便和 App 侧对照。
# 更早一版是 ffd57b46…（9640 B）—— 日志位置从写死的 /data/wayland/mkpool.log
# 改成"用 /proc/self/exe 反查自己旁边"，路径变了、二进制就变了。
#
# ⚠️ vdrm_mkpool 本身【要入库】（KMI 无关的小二进制，CI 装配模块 zip 时要用），
#    所以改了 mkpool.c 之后：重编 → 更新上面这个哈希 → 把新产物一起提交。

CC      ?= aarch64-linux-android-clang
API     ?= 36
TARGET   = vdrm_mkpool

all: $(TARGET)

$(TARGET): mkpool.c
	$(CC) -target aarch64-linux-android$(API) -o $@ mkpool.c \
	      -L/system/lib64 -landroid -lnativewindow -llog

clean:
	rm -f $(TARGET)
