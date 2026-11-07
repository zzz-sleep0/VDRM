## 前置声明
理论上该模块仅适用于5.10-android13到6.12-android16对应KMI内遵循GKI/OKI标准的内核。

对于不同的内核，本模块的处理方式和Dere3046大佬的_patchHarmony项目处理方式类似，如果那个项目在你的内核上可以运行，理论上这个项目也可以

如果你的内核声明已经达到该模块支持的KMI，但内核本身并未遵循GKI/OKI标准，且没有对应的_patchHarmony部署成功先例，**先别刷.zip模块**：按文末「注意」里的方式单独 `insmod` 试一次，否则请做好音量键救砖甚至格式化data分区的准备！

对于KMI不在支持范围内的内核，该模块不会加载内核模块因此不会实质性生效

# VDRM

手机上的虚拟显示器：内核模块给出 `/dev/dri/card1`，容器里的合成器（niri / KWin /
Hyprland / sway）直接画在上面，App 负责显示、虚拟键鼠和声音。默认 1920x1080@120。

## 1. 装模块

刷 `vdrm-lkm-*.zip`，重启手机。（内核不符合 GKI 标准的话，先做文末「兼容性自测」，
别直接刷 zip。）

```sh
su -c 'lsmod | grep ^vdrm'
ls -l /dev/vdrm        # crw-rw-rw-
```

## 2. 建容器

改 `container.config`（实测的 Fedora 43 配置）：

```ini
enable_hw_access=0
enable_gpu_mode=1
allow_sandboxing=1
privileged=nomask
bind_mounts=/dev/dri/card1:/dev/dri/card1,/dev/input:/dev/input,/dev/snd:/dev/snd
```

改 `.env`：

```ini
MESA_LOADER_DRIVER_OVERRIDE=msm
LIBSEAT_BACKEND=noop
```

`enable_hw_access` 保持 0：开了之后容器里 `/sys` 可写，装驱动可能把整机软重启。
代价是容器里没有 udev，所以下一步用 systemd oneshot 顶替。

## 3. 容器里装节点服务

用 systemd 顶替 udev：把下面这个脚本装成 oneshot，开机建 `/dev/input`、
`/dev/snd` 和 `/run/udev/data` 属性。

```sh
# 脚本（也在仓库 tools/ 里）
curl -L -o /tmp/vdrm-container-setup.sh \
  https://raw.githubusercontent.com/zzz-sleep0/VDRM/main/tools/vdrm-container-setup.sh
install -m 755 /tmp/vdrm-container-setup.sh /usr/local/sbin/vdrm-container-setup
cat > /etc/systemd/system/vdrm-input.service <<'EOF'
[Unit]
Description=vdrm: only expose the virtual input devices
After=local-fs.target
Before=display-manager.service

[Service]
Type=oneshot
RemainAfterExit=yes
ExecStart=/usr/local/sbin/vdrm-container-setup

[Install]
WantedBy=multi-user.target
EOF
systemctl enable --now vdrm-input
```

它依次做三件事：`mount --make-rprivate /dev` 后用 tmpfs 顶掉 `/dev/input`、
`/dev/snd`；mknod 出 vdrm 三台虚拟设备（输入 `0666`，声卡 `root:audio 0660`）；
写 `/run/udev/data/c13:<minor>` 的 `ID_INPUT*`（缺了 libinput 会直接跳过设备）。

```sh
systemctl is-active vdrm-input      # active
ls /dev/input/                      # event7/8/9 + vdrm-kbd/ptr/touch
```

⚠️ 别在容器里 `rm`/`chmod` `/dev/input`、`/dev/snd`：那是宿主机目录的 bind mount，
同一批 inode，删掉宿主机上就真没了（会让 Android 失去触摸）。脚本会先挂 tmpfs 并
用自己的 fsname 确认挂上才动手。

模块重载后 `eventN` 编号会变：`systemctl restart vdrm-input`。

## 4. 装 Mesa / GBM / 合成器

```sh
# Mesa（Fedora 43；Arch 换对应包）
curl -L -o /tmp/mesa.tar.gz https://github.com/lfdevs/mesa-for-android-container/releases/download/mesa-27.0.0-devel-20261025-r1/mesa-for-android-container_27.0.0-devel-20261025-r1_fedora_43_arm64.tar.gz
tar -xzf /tmp/mesa.tar.gz -C /

# GBM 后端（Fedora 是 /usr/lib64/gbm，Arch/Debian 是 /usr/lib/gbm）
cp vdrm_gbm.so /usr/lib64/gbm/

# 合成器
dnf install -y plasma-workspace kwin kscreen
```

## 5. 起桌面

做成 systemd 服务，从任何入口（容器控制台 / 管理器 shell）都能起，也不怕关掉终端：

```sh
cat > /etc/systemd/system/vdrm-kde.service <<'EOF'
[Unit]
Description=VDRM: KDE session on the virtual display
After=systemd-user-sessions.service vdrm-input.service

[Service]
Type=simple
Environment=XDG_RUNTIME_DIR=/tmp/xdg
Environment=KWIN_DRM_DEVICES=/dev/dri/card1
Environment=MESA_LOADER_DRIVER_OVERRIDE=msm
ExecStartPre=/bin/sh -c "install -d -m 700 /tmp/xdg"
ExecStart=/usr/bin/dbus-run-session -- /usr/sbin/startplasma-wayland
Restart=always
RestartSec=3

[Install]
WantedBy=multi-user.target
EOF
systemctl daemon-reload
systemctl enable --now vdrm-kde
```

日常操作就是 `systemctl start vdrm-kde` / `stop` / `status`，开机（容器启动）自动起。

确认出图：

```sh
pgrep -c -x kwin_wayland                        # 1
cat /sys/module/vdrm/parameters/frames_pushed   # 动鼠标要涨
cat /sys/module/vdrm/parameters/active
```

换合成器就把 `ExecStart` 换成 `niri` / `Hyprland` / `sway`。

## 6. 装 App

装 `vdrm-debug.apk`，打开，给通知权限和 root 授权，保持运行。

| 通知按钮 | 作用 |
|---|---|
| `鼠标` | 手机触摸当鼠标（相对位移） |
| `触控` | 手机触摸当触控屏（按哪点哪） |
| `刷新` | 重读内核参数 |

## 7. 切分辨率

```sh
kscreen-doctor output.1.mode.2                 # KWin（-o 先看列表）
niri msg output Virtual-3 mode 1264x2780       # niri
swaymsg output Virtual-3 mode 1264x2780        # sway
```

开机默认档在模块目录 `mode.conf`：

```
mode=1920x1080
#refresh=120
#dpi=256
```

## 8. 声音

```sh
aplay -l | grep vdrm
```

## 注意：KMI 对得上，也可能不兼容

`uname -r` 在支持的 KMI 里、模块里也有对应的 `.ko`，不代表一定能加载：厂商魔改过的
内核可能 `insmod` 报 `Unknown symbol`、`probe` 失败甚至当场 panic。**先别刷 zip**，
按下面六步单独验一遍，哪一步不对就 `rmmod` 收手。

### 1. 记下现状

```sh
uname -r                                  # 决定的 KMI，比如 6.1.x-android14-...
for p in /sys/class/input/event*/device/name; do echo "$p: $(cat $p)"; done
```

把 `eventN` 列表抄下来——模块加载后会多出 `vdrm-kbd` / `vdrm-ptr` / `vdrm-touch` 三个，随后的/dev/input/event*的设备号也会多处三个，跟着下面**排除设备**处理。

### 2. 只解出 .ko，单独 insmod

```sh
mkdir -p /data/local/tmp/vdrm && cd /data/local/tmp/vdrm
unzip /sdcard/vdrm-lkm-v3.*.zip 'vdrm-*.ko'      # 只取 .ko，别的先别装
su -c 'insmod /data/local/tmp/vdrm/vdrm-android14-6.1.ko'
```

### 3. 看 dmesg 走到哪一步

```sh
su -c 'dmesg | grep vdrm' | tail -30
```

正常应该依次出现（缺哪个，就是卡在哪一步）：

```
vdrm: registered
vdrm: /dev/vdrm registered (minor=..)
vdrm: stage=register-ok
input: vdrm-kbd as /devices/virtual/input/inputNN
input: vdrm-ptr ...
input: vdrm-touch ...
vdrm: stage=input-ok
vdrm: audio ready (/dev/vdrm-audio out 48000 Hz 2 ch ring 524288, ...)
vdrm: bring-up ok (dev=...)
```

`insmod` 直接 panic / 手机重启 = 这个内核不兼容，**到此为止**。

### 4. 检查节点

```sh
ls -l /dev/vdrm /dev/vdrm-audio /dev/vdrm-audio-in     # 三个都在
ls -l /dev/dri/card1                                   # 226,1
cat /proc/asound/cards | grep vdrm                     # 有一张 vdrm 声卡
```

节点权限在手工 `insmod` 时是默认值，验证阶段先给开：

```sh
su -c 'chmod 666 /dev/vdrm /dev/vdrm-audio /dev/vdrm-audio-in'
su -c 'chcon u:object_r:gpu_device:s0 /dev/vdrm'
```
**设备排除**

```sh
# 只对 vdrm 那三个节点做
chown root:root /dev/input/event*     
chmod 0600 /dev/input/event*
```

### 5. 用 App 打通一条帧

装 APK、打开、授权，然后：

```sh
su -c 'dmesg | grep -E "vdrm: (app eventfd|REGISTER)" | tail -8'
```

应该看到 `app eventfd registered` 和 **6 条** `REGISTER id=1..6 1920x1080`。

如果没有问题，而且急于想要看到效果，可以按照上面说的打开一个合成器看看有没有画面。

### 6. 通过之后再刷 zip

在模块管理器刷 `vdrm-lkm-*.zip`，重启。之后按第 2 步往后正常部署。



### 容器侧不用再排除

模块加载后自己把虚拟键鼠的 `/dev/input/event*`（比加载前多出来的那几个）换成 0600
root 节点，Android 的 InputReader 打不开就看不见；容器侧装好第 3 步那个 service 后，
容器里只有 vdrm 三个虚拟设备，手机真实的触摸/电源键/指纹看不见（root 跑合成器也一样）。

## 鸣谢

- Goldzxcbug、Superturtlee：项目引路与参考项目
- Dere3046：普适化工具与排查思路
- lfdevs：[mesa-for-android-container](https://github.com/lfdevs/mesa-for-android-container)：硬件加速驱动
- [anland](https://github.com/SuperTurtleDev/anland)：初期思路与键鼠触控优化
