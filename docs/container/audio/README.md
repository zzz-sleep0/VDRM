# 容器里的声音：让 PipeWire 真的开那张卡（Fedora 43 / PipeWire 1.4 实测）

VDRM 的声音是**按需**的，链路只有三步：

1. 容器里有人打开 `/dev/snd/pcmC0D0p`；
2. 内核把 `g_play_users` 加一，并 eventfd 叫醒 App；
3. App 看到 `play_users > 0` 才起 AAudio；PCM 关掉后两秒再停。

所以容器里必须有一个声卡服务，**会开也会关**这个 PCM。PipeWire 空闲时会把
节点挂起（关掉 PCM），正好满足第三条；但 Fedora 的默认配置在这个容器里有两道坎。

## 坎一：`ConditionUser=!root`，单元被直接 skip

```
/usr/lib/systemd/user/pipewire.socket:3      ConditionUser=!root
/usr/lib/systemd/user/pipewire.service:17    ConditionUser=!root
/usr/lib/systemd/user/pipewire-pulse.socket:3
/usr/lib/systemd/user/pipewire-pulse.service:17
```

KDE 走的是 SDDM 的 root 自动登录，于是这四个单元条件不满足，**不是失败，是
skip**。表现是 `systemctl --user is-active pipewire.socket` 为 inactive、
`/run/user/0` 下没有 `pipewire-0`，KDE 里没有音频设备。

修法：四个单元各放一份清空条件的 drop-in（见 `10-vdrm-root.conf`），空赋值会把
条件列表整个清掉。

## 坎二：WirePlumber 在容器里枚举不到 ALSA 卡

容器的 `/run/udev` 是 `vdrm-udev` 手写的数据，没有 udev 冷插拔；WirePlumber 的
ALSA 监视器虽然起来了（日志里有 `Activating ALSA monitor`），但一个设备都发现
不了，`wpctl status` 里 Devices/Sinks 全空。

修法：不走 udev 发现，直接用 PipeWire 的静态节点语法把 adapter 挂到
`hw:0,0` 上（见 `10-vdrm-sink.conf`）。空闲挂起交给 PipeWire 自己。

> 注意 `api.alsa.path` 写的是容器里的卡号。容器只 bind 了 vdrm 的节点，
> 所以是 `hw:0,0`；如果同时 bind 了别的卡，先 `cat /proc/asound/cards` 确认。

## 装

```sh
# 1) 清掉 ConditionUser
install -d /etc/systemd/user/{pipewire.socket,pipewire.service,pipewire-pulse.socket,pipewire-pulse.service}.d
for u in pipewire.socket pipewire.service pipewire-pulse.socket pipewire-pulse.service; do
    install -m 644 10-vdrm-root.conf /etc/systemd/user/$u.d/10-vdrm-root.conf
done

# 2) 静态 sink
install -d /etc/pipewire/pipewire.conf.d
install -m 644 10-vdrm-sink.conf /etc/pipewire/pipewire.conf.d/10-vdrm-sink.conf

# 3) 生效（不需要重启容器）
systemctl --user daemon-reload
systemctl --user start pipewire.socket pipewire-pulse.socket
```

## 验

```sh
export XDG_RUNTIME_DIR=/run/user/0

systemctl --user is-active pipewire.socket pipewire.service wireplumber.service pipewire-pulse.socket
wpctl status | sed -n '/Audio/,/Video/p'      # Sinks 里应有一项 "VDRM virtual audio"
```

播一段东西，同时看内核计数：

```sh
# 容器里
speaker-test -D hw:0,0 -c 2 -t sine -l 1          # 裸 ALSA，先证明卡是开的
# 或者用 PipeWire：pw-play /tmp/tone.wav

# 宿主上
cat /sys/module/vdrm/parameters/play_users          # 播放期间 = 1
logcat -s VDRM | grep "audio: AAudio"               # 应出现 AAudio output started
```

停播后 `play_users` 回到 0，两秒内 App 打印 `AAudio output stopped (PCM closed/idle)`。

## 排错

| 现象 | 原因 |
|---|---|
| `play_users` 恒 0，但 `aplay` 能出声 | 内核模块是旧版：`snd_pcm_hw_constraint_integer` 成功返回 1，旧代码 `if (rc)` 当失败提前 return。见 commit `fix(audio)` |
| `wpctl status` 里没有 Sinks | 静态 sink 没生效：`api.alsa.path` 的卡号不对，或 `pipewire.conf.d` 目录名写错 |
| `systemctl --user is-active pipewire.socket` 是 inactive | 还是 `ConditionUser` 没清掉；`systemctl --user status pipewire.socket` 会打印 `Condition ... was not met` |
| `aplay` 报 `Device or resource busy` | PipeWire 已经开着那张 PCM，属于正常（按需链路的"开"状态） |

## 前提

- 容器运行时已 bind `/dev/snd/controlC0`、`pcmC0D0p`、`pcmC0D1c`（不需要 `timer`，实测去掉照样出声）；
- `vdrm-udev.service` 已写好 `/run/udev/data`（`audio_device` 标签、seat 属性）；
- 内核模块的 `vdrm_audio.c` 是修过约束返回值之后的版本。
