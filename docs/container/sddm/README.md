# 用 SDDM 在容器启动阶段拉起 KDE（尽量原生）

这套配置在 Fedora 43 / KWin 6.7.5 / SDDM 0.21.0 上实测通过。容器重启后：

- SDDM 自动登录 root；
- session 的 `Service=sddm-autologin`、`Leader=sddm-helper`；
- KWin / ksmserver / plasmashell / kded 由 **`systemd --user`** 的
  `plasma-kwin_wayland.service`、`plasma-ksmserver.service`、
  `plasma-plasmashell.service` 等 user unit 拉起；
- `plasma_session` 不再报 `extremely legacy forking mode`；
- session 是 `VTNr=1 / Seat=seat0 / Active=yes`，输入通过 logind
  `TakeDevice` 进来。

前提：内核模块提供虚拟 VT；容器运行时已 bind `/dev/tty0..6`、`/dev/dri/card1`、
`/dev/input/eventN`、`/dev/snd/*`。`vdrm-udev.service` 只写 `/run/udev/data`/tags，
不 mknod、不 mount、不 mask。

## 1. 装文件

```sh
install -m 755 tools/vdrm-sddm-start.sh /usr/local/bin/vdrm-sddm-start
install -m 644 docs/container/sddm/vdrm-start.desktop \
  /usr/share/wayland-sessions/vdrm-start.desktop

install -d /etc/sddm.conf.d
install -m 644 docs/container/sddm/10-vdrm.conf \
  /etc/sddm.conf.d/10-vdrm.conf

install -d /etc/systemd/system/sddm.service.d
install -m 644 docs/container/sddm/override.conf \
  /etc/systemd/system/sddm.service.d/override.conf

# SDDM 进程（greeter）和 user session 都要有 MESA/KWIN
cp docs/container/sddm/environment /etc/environment
install -d /etc/environment.d
cp docs/container/sddm/environment /etc/environment.d/10-vdrm.conf

# 让 systemd --user 读新的 environment.d
systemctl restart user@0.service
```

`/etc/environment` 给 PAM/session；`/etc/environment.d/10-vdrm.conf`
给 `systemd --user`，后者会把变量传给 `plasma-kwin_wayland.service`。

SDDM 以 root 跑（`sddm.service` 里没有 `User=`），不需要额外加组；
自动登录的用户也是 root，`/dev/dri/card1`、`/dev/input/eventN` 都读得到。

## 2. 关掉自定义 vdrm-kde，开 SDDM

```sh
systemctl daemon-reload
systemctl enable sddm
systemctl disable --now vdrm-kde
```

`vdrm-kde.service` 还是可以留着当 fallback；两个都 enable 会抢 card1。

设备节点由运行时 bind；`vdrm-udev.service` 在 logind 之前只写
`/run/udev/data` 和 `/run/udev/tags`，不碰 `/dev`。

## 3. 重启容器验证

```sh
systemctl reboot      # 或从宿主 droidspaces restart
```

起来后：

```sh
systemctl is-active sddm vdrm-input
pgrep -a kwin_wayland
loginctl show-session c1 -p Seat -p VTNr -p Active -p Service

# 关键：KDE 的 user unit 是否真的 native 起来了
XDG_RUNTIME_DIR=/run/user/0 DBUS_SESSION_BUS_ADDRESS=unix:path=/run/user/0/bus \
  systemctl --user list-units | grep -E 'plasma-(kwin|ksmserver|plasmashell|kded)'
```

期望：`sddm` active；KWin/ksmserver/plasmashell 的父进程是
`systemd --user`（不是 `plasma_session`）；上一条能看到
`plasma-kwin_wayland.service` / `plasma-ksmserver.service` /
`plasma-plasmashell.service` active；KWin 日志里没有
`Could not determine the active graphical session`，也没有
`Failed to open /dev/input/eventN device (No such device)`。

## 两个坑（不修的话就是“SDDM 起来了但 KDE 秒退/退化”）

1. **SIGHUP 杀掉会话包装**。SDDM 的 `UserSession::childModifier()` 对 Wayland
   user session 会先把 `/dev/ttyN` 设成 stdio、`setsid()`、`TIOCSCTTY`，再 exec
   会话命令；`dbus-run-session`/KDE 的 fork 会让内核给旧 session leader 发
   SIGHUP，`/etc/sddm/wayland-session` 这个直接子进程被收掉，SDDM 就关掉整个
   PAM session。`tools/vdrm-sddm-start.sh` 里的 `trap '' HUP` 是必须的；忽略
   信号会跨 exec 保留。

2. **`/run/udev/tags` 不是 `/run/udev/data`**。`vdrm-udev.service` 会同时写
   data 和 tags；只写 data 时 logind 的 `master-of-seat` 枚举不到。它必须在
   logind 之前跑，但只写 `/run/udev`，不 mknod/mount/mask。

## 关于 DBus：别再用私有 `dbus-run-session`

第一版为了绕开 SIGHUP，在会话脚本里 `unset DBUS_SESSION_BUS_ADDRESS` 再起
`dbus-run-session`。这能让 KDE 起来，但 `plasma_session` 看不到
`systemd --user`，KWin/ksmserver/plasmashell 全部退回
`extremely legacy forking mode`，部分 user unit 驱动的服务就不按原生路径走。

现在保留 SDDM/PAM 给的 `/run/user/0/bus`，只做 `trap '' HUP`，用
`plasma-dbus-run-session-if-needed`：有 bus 就直接 exec startplasma，没有才
自己起 `dbus-run-session`。这样 `systemd --user` 的 KDE units 才是原生的。

另外，`/etc/environment` 里的 `KWIN_DRM_DEVICES` / `MESA` 不能只放在
`vdrm-kde.service` 里：SDDM 的 user session 不走那个 unit；同时还要有
`/etc/environment.d/10-vdrm.conf`，否则 `plasma-kwin_wayland.service`
由 `systemd --user` 拉起来时拿不到这两个变量。
