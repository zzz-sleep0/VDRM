#!/bin/sh
# VDRM — SDDM 的 Wayland 用户会话包装。
#
# 这个包装只做三件事，其余全部走 SDDM/logind/systemd --user 的原生路径：
#
#   1. trap '' HUP
#      SDDM 的 UserSession::childModifier() 对 Wayland user session 会先把
#      /dev/ttyN 设成 stdio、setsid()、TIOCSCTTY，再 exec 会话命令。会话里
#      dbus-run-session/plasmashell 的 fork 会让内核给旧 session leader 发
#      SIGHUP；直接子进程（这个脚本）被收掉后 SDDM 认为会话结束，会关掉
#      整个 PAM session 并连 KWin 一起杀。忽略 SIGHUP 会跨 exec 保留。
#
#   2. unset LIBSEAT_BACKEND
#      容器 .env 里常有 LIBSEAT_BACKEND=noop（niri/Hyprland 的历史配置）。
#      KWin 6.7 不链 libseat，但清掉它避免误导后续排查。
#
#   3. plasma-dbus-run-session-if-needed
#      这里不 unset DBUS_SESSION_BUS_ADDRESS，也不另起 dbus-run-session：
#      SDDM/PAM 已经给了 /run/user/0/bus（systemd user bus）。保留它，
#      startplasma/plasma_session 才能看到 systemd --user，把
#      plasma-kwin_wayland / plasma-ksmserver / plasma-plasmashell 作为
#      user unit 起来，而不是 "extremely legacy forking mode"。
#      如果环境里确实没有 session bus，这个 helper 自己会兜底起
#      dbus-run-session。
trap '' HUP

unset LIBSEAT_BACKEND

export DESKTOP_SESSION=plasma

exec /usr/libexec/plasma-dbus-run-session-if-needed /usr/bin/startplasma-wayland
