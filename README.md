# VDRM

手机上的虚拟显示器：内核模块给出 `/dev/dri/card1`，容器里的合成器（niri / KWin /
Hyprland / sway）直接画在上面，App 负责显示、虚拟键鼠和声音。

默认 1920x1080@120。

## 1. 装模块

模块管理器刷 `vdrm-lkm-*.zip`，重启手机。检查：

```sh
su -c 'lsmod | grep ^vdrm'
ls -l /dev/vdrm        # crw-rw-rw-
```

## 2. 建容器

Droidspaces 建容器（下面是实测的 Fedora 43 配置，改 `container.config` 这五行）：

```ini
enable_hw_access=0
enable_gpu_mode=1
allow_sandboxing=1
privileged=nomask
bind_mounts=/dev/dri/card1:/dev/dri/card1,/dev/input:/dev/input,/dev/snd:/dev/snd
```

容器 `.env`：

```ini
MESA_LOADER_DRIVER_OVERRIDE=msm
LIBSEAT_BACKEND=noop
```

## 3. 容器里装三样

```sh
# Mesa（Fedora 43；Arch 换对应包）
curl -L -o /tmp/mesa.tar.gz https://github.com/lfdevs/mesa-for-android-container/releases/download/mesa-27.0.0-devel-20261025-r1/mesa-for-android-container_27.0.0-devel-20261025-r1_fedora_43_arm64.tar.gz
tar -xzf /tmp/mesa.tar.gz -C /

# GBM 后端（Fedora 是 /usr/lib64/gbm，Arch/Debian 是 /usr/lib/gbm）
cp vdrm_gbm.so /usr/lib64/gbm/

# 合成器
dnf install -y plasma-workspace kwin kscreen
```

再装虚拟输入/声卡节点（`tools/vdrm-container-setup.sh`，装成 `vdrm-input.service`）：

```sh
systemctl enable --now vdrm-input
```

## 4. 起桌面

```sh
install -d -m 700 /tmp/xdg
export XDG_RUNTIME_DIR=/tmp/xdg
dbus-run-session -- startplasma-wayland
```

换了合成器就换最后一行（`niri` / `Hyprland` / `sway`）。确认出图：

```sh
cat /sys/module/vdrm/parameters/frames_pushed   # 动鼠标，这个数要涨
cat /sys/module/vdrm/parameters/active
```

## 5. 装 App

装 `vdrm-debug.apk`，打开，给通知权限和 root 授权，**保持运行**。通知里两个按钮：

| | |
|---|---|
| `刷新` | 重读内核参数 |
| `命令` | `input 0` 触摸当鼠标 / `input 1` 触摸当触控屏 / `commit` 广播参数 |

## 6. 切分辨率 / 缩放

在合成器里切：

```sh
kscreen-doctor output.1.mode.2                 # KWin（-o 先看列表）
niri msg output Virtual-3 mode 1264x2780       # niri
swaymsg output Virtual-3 mode 1264x2780        # sway
```

缩放写在合成器自己的配置里（niri `output "Virtual-3" { scale 1.5 }`，
Hyprland `monitor=Virtual-3,preferred,auto,1.5`，KWin 用系统设置）。

开机默认档写在模块目录的 `mode.conf`：

```
mode=1920x1080
#refresh=120
#dpi=256
```

## 7. 声音

容器里当普通输出设备用：

```sh
aplay -l | grep vdrm
```
