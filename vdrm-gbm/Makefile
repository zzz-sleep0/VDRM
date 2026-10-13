# vdrm_gbm.so —— 让 GBM 在 vdrm(card1) 上走 DRM dumb 分配路径
#
# 依赖两个头：
#   gbm_backend_abi.h  ← 本目录（Mesa 上游 src/gbm/backends/ 的原样拷贝）
#   gbm.h              ← 发行版 mesa 包自带（/usr/include/gbm.h），不用管
#
# 产物只依赖 libc，不链接 libgbm/libdrm —— 运行时才 dlopen dri_gbm.so。

CC      ?= gcc
CFLAGS  ?= -shared -fPIC -O2 -Wall
TARGET   = vdrm_gbm.so

all: $(TARGET)

$(TARGET): vdrm_gbm.c gbm_backend_abi.h
	$(CC) $(CFLAGS) -o $@ vdrm_gbm.c

clean:
	rm -f $(TARGET)
