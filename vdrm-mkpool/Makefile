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
#   md5sum vdrm_mkpool  →  8f7cf89bc6d5531c6e482512c2d8bdde   (10144 B)
#
# 这个值随源码变。上一版是 ffd57b46…（9640 B）—— 日志位置从写死的
# /data/wayland/mkpool.log 改成"用 /proc/self/exe 反查自己旁边"，路径变了、
# 二进制就变了。改了 mkpool.c 记得重跑一次、把这里更新掉。

CC      ?= aarch64-linux-android-clang
API     ?= 36
TARGET   = vdrm_mkpool

all: $(TARGET)

$(TARGET): mkpool.c
	$(CC) -target aarch64-linux-android$(API) -o $@ mkpool.c \
	      -L/system/lib64 -landroid -lnativewindow -llog

clean:
	rm -f $(TARGET)
