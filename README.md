# VDRM

把一台 Android 手机变成一台 **Linux 桌面**：内核模块造一块虚拟显示器（`/dev/dri/card1`），
容器里的合成器（niri / KWin / Hyprland / sway）往它上面画，画面由 App 显示在手机屏幕上。
同时提供虚拟键鼠和虚拟声卡。

默认 **1920x1080@120**，比例不合适时在合成器里切档。

---

## 1. 准备

| | |
|---|---|
| 手机 | 已 root（KernelSU / Magisk / APatch），KMI 在 5.10-android13 ~ 6.12-android16 之间 |
| 容器 | [Droidspaces](https://github.com/ravindu644/Droidspaces-OSS) |
| 合成器 | 容器里装一个（niri / KWin / Hyprland / sway 都验过） |

---

## 2. 拿文件

去 Releases 下最新的 `weekly-YYYY-Www`：

```
vdrm-lkm-*.zip        模块（.ko + mkpool + 启动脚本）
vdrm_gbm.so           容器里的 GBM 后端
vdrm-debug.apk        App
```

---

## 3. 装模块

在模块管理器里刷 `vdrm-lkm-*.zip`，**重启手机**。

重启后检查：

```sh
su -c 'lsmod | grep ^vdrm'
ls -l /dev/vdrm            # crw-rw-rw-，标签 gpu_device
```

---

## 4. 建容器

装好 Droidspaces 后建容器（下面以 Fedora 43 为例），编辑它的 `container.config`：

```ini
enable_hw_access=0
enable_gpu_mode=1
allow_sandboxing=1
privileged=nomask
bind_mounts=/dev/dri/card1:/dev/dri/card1
```

容器 `.env`：

```ini
MESA_LOADER_DRIVER_OVERRIDE=msm
LIBSEAT_BACKEND=noop
```

启动容器。

---

## 5. 容器里装依赖

```sh
# 1) Mesa for Android Container（Fedora 43；Arch 用对应的包）
curl -L -o /tmp/mesa.tar.gz \
  https://github.com/lfdevs/mesa-for-android-container/releases/download/mesa-27.0.0-devel-20261025-r1/mesa-for-android-container_27.0.0-devel-20261025-r1_fedora_43_arm64.tar.gz
tar -xzf /tmp/mesa.tar.gz -C /

# 2) GBM 后端：放发行版的 gbm 目录
#    Fedora/openSUSE: /usr/lib64/gbm/    Arch/Debian: /usr/lib/gbm/
cp vdrm_gbm.so /usr/lib64/gbm/

# 3) 合成器（这里是 KDE）
dnf install -y plasma-workspace kwin kscreen

# 4) 虚拟输入/声卡节点（见 tools/vdrm-container-setup.sh）
systemctl enable --now vdrm-input
```

---

## 6. 起桌面

```sh
install -d -m 700 /tmp/xdg
export XDG_RUNTIME_DIR=/tmp/xdg
dbus-run-session -- startplasma-wayland
```

其它合成器直接跑标准命令：`niri` / `Hyprland` / `sway`。

确认在出图：

```sh
cat /sys/module/vdrm/parameters/frames_pushed    # 动一下鼠标，这个数要涨
cat /sys/module/vdrm/parameters/active            # 合成器真正上屏的尺寸
```

---

## 7. 装 App

装 `vdrm-debug.apk`，打开，给通知权限和 root 授权。**App 要一直开着**，画面和声音都靠它。

常驻通知就是控制台：

| 操作 | 作用 |
|---|---|
| `刷新` | 重读内核参数 |
| `命令` → `input 0` / `input 1` | 手机触摸当鼠标 / 当触控屏 |
| `命令` → `commit` | 手改了 `/sys/module/vdrm/parameters/*` 之后广播 |

---

## 8. 切分辨率 / 刷新率 / 缩放

档位由驱动广告，**在合成器里切**：

```sh
kscreen-doctor output.1.mode.2                 # KWin（先 -o 看列表）
niri msg output Virtual-3 mode 1264x2780       # niri
swaymsg output Virtual-3 mode 1264x2780        # sway
```

缩放同理，写在合成器自己的配置里（niri: `output "Virtual-3" { scale 1.5 }`，
Hyprland: `monitor=Virtual-3,preferred,auto,1.5`，KWin: 系统设置里的缩放）。

开机默认档写在模块目录的 `mode.conf`：

```
mode=1920x1080
#refresh=120
#dpi=256
```

---

## 9. 声音

模块注册一张虚拟声卡（48 kHz / 立体声 / S16_LE），容器里当普通输出设备用：

```sh
aplay -l | grep vdrm
```

---

## 10. 自己构建（可选）

```sh
# 模块 zip（需要六档 .ko 和 mkpool）
python3 make-module.py

# 单个 KMI 的 .ko
cd vdrm-ko-ddk
make -C /opt/ddk/kdir/android14-6.1 M=$PWD ARCH=arm64 LLVM=1 LLVM_IAS=1 \
     CC=clang LD=ld.lld AR=llvm-ar NM=llvm-nm OBJCOPY=llvm-objcopy \
     OBJDUMP=llvm-objdump STRIP=llvm-strip modules

# GBM 后端
cd vdrm-gbm && make

# APK
gradle assembleDebug
```
