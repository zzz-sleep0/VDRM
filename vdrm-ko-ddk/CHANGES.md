# 本版本与原始基线的差异

> 这里记的是**为什么这么改**：每一项的根因、证据、修法。
> 只想把模块跑起来的话看 `README.md` 就够了，不必读这份。

## 本版本第五十四项：`vdrm_gbm.so` 进 CI 构建 + 失败要说人话

`vdrm_gbm.so` 是容器里的 **GBM 后端**（不是普通链接库）：libgbm 按 card1 的驱动名
`vdrm` 去后端目录找 `vdrm_gbm.so`，它再把一切转发给发行版的 `dri_gbm.so`，只把
分配改走 DRM dumb 路径。

### 1. 它其实是个构建产物，CI 却一直在 cp

`.gitignore` 里有 `*.so` —— 仓库里**没有**这个文件。而 `release.yml` 的装配步骤是
`cp vdrm-gbm/vdrm_gbm.so assets/`，干净 checkout 上这一步必然失败（本地能过是因为
工作树里恰好有一份编好的）。现在装配前先 `make -C vdrm-gbm` 现编一次。

顺带把 release job 固定在 `ubuntu-22.04`：**构建机的 glibc 就是产物的 glibc 下限**，
用 `ubuntu-latest`（24.04）会让它要 `GLIBC_2.38`，Debian 12 / Ubuntu 22.04 直接
加载失败。当前产物要 `GLIBC_2.34`（`readelf -V` 实测）。

### 2. 失败不再静默

`dlopen("dri_gbm.so")` 失败、或 loader 的 `core_version` 和本后端的
`GBM_BACKEND_ABI_VERSION` 不一致，现在都往 stderr 打一行原因。以前这两种情况的表现
都是"合成器起不来"，没有任何线索。

### 3. README 补了兼容性三条件

glibc ≥ 2.34 / 发行版 Mesa 带 `dri_gbm.so` / Mesa 的 GBM 后端 ABI 只做追加式演进。
并说清它**不依赖** mesa-for-android-container（那套是渲染侧 card0/EGL，这个 shim
只管 card1 的 dumb buffer 分配）。

## 本版本第五十三项：模块瘦身（配置面全部交给通知）

做完第五十二项之后回头看模块：589 行 shell 里有一大半是通知已经能做的事，
还有一些跟 VDRM 根本无关。按"配置面全部交给用户态通知"重新划边界。

### 1. 删 `action.sh`（80 行）

音量键选分辨率 → 写 `mode.conf`。通知里的 `[全面屏]/[黑边屏]` 按钮做的完全是
同一件事，而且是**实时生效**、不用等重启。模块管理器里那个 Action 键不再需要。

### 2. `adbd_keep.sh` 移出模块（80 行）

每 20 秒 `stop adbd; setprop service.adb.tcp.port 5555; stop adbd; start adbd`。
这是本机调试环境的需要，跟 VDRM 没有任何关系 —— 陌生人装上看到模块每 20 秒重启
adbd，第一反应只会是"这东西在干什么"。挪到仓库 `tools/adbd_keep.sh`，自用请自装，
不再随 zip 发布（`service.sh` 里装/起它的那 20 行一起删掉）。

### 3. 通知回写 `mode.conf`（A 方案）

原来通知里改的档位不落盘，重启就回到 `mode.conf` —— 用户会当成 bug。现在：

- `[全面屏]/[黑边屏]`、`切横竖` 切成功之后写 `mode=<新档>`；
- `refresh N` 成功之后写 `refresh=<N>`；
- 只替换/追加对应那一行，注释和其它键原样保留。

于是边界清晰了：**通知 = 唯一配置入口，`mode.conf` = 它的持久化记录（也是开机
insmod 的种子）**。README 里"用 Action 切"整节删掉，换成"开机默认档：mode.conf"。

### 4. 没动的部分（这些不能砍）

- `post-fs-data.sh`：加载 .ko + 修 `/dev/vdrm` 权限标签 + 把虚拟键鼠节点藏起来
  —— **必须早于 system_server**，晚一步就出幽灵触摸；
- `service.sh` 的建池：合成器可能先于 App 启动，池必须在那之前就绪；
- `common.sh`：KMI 匹配（文件名就是 KMI）、三个 insmod 参数、两个节点函数；
- `status.sh`：不开 App 时，管理器那行状态是唯一的可见状态。

模块从 **589 行 → 约 290 行**（`action.sh` 80 + `adbd_keep.sh` 80 + `service.sh`
里 20 行 + README 相应章节）。

## 本版本第五十二项：`safe_present` 退路 + 日志降噪 + 文档对齐

第五十项用"不等 SF present"换了 88fps，代价是合成器可能覆写还在显示的显存。
这条路径在**本机**实测零撕裂，但别人装上不该只有"整包退回上一版"这一个选项。

### 1. `safe_present`（模块参数，0644）

```sh
echo 1 > /sys/module/vdrm/parameters/safe_present   # 保守：等 present 才完成 flip-done
echo 0 > /sys/module/vdrm/parameters/safe_present   # 默认：下一个 vblank 就完成
```

保守模式把事件重新挂到池槽上、等 App 报 present（500ms 兜底），也就是
第四十五项那套。两个计数器 `flip_safe_present_done` / `flip_safe_fallback`
用来确认它确实在工作。

实测（连续注入鼠标位移 8 秒）：

| 模式 | 帧率 | `frames_reuse` |
|---|---|---|
| 默认（0） | 63 fps | +281 |
| 保守（1） | 13 fps | **+0** |

保守模式慢得多（它把 SF 的 present 又放回了关键路径），所以**只作排障/兼容用**，
不当默认。但它确实做到了"绝不覆写"。

### 2. 日志降噪

- 内核：`mode prepared` / `get_modes` / `App 上报屏幕` / `app ready` / `dropped stale`
  五条每次换档都刷的降到 `pr_debug`。留下 `pipe enable`（"合成器到底换没换"的
  硬证据）、`commit ->`、注册/错误。
- App：心跳 `hb loop` 只打前 3 轮（原来 8 轮），每块 buffer 的 `forged[i]` 降到
  `LOGD`（`forge_pool: N forged` 的汇总保留）。`STATS` 那行保留 —— 它是排障主力。

### 3. 文档与仓库清理

- README 的"已知局限"重写：删掉过时的"连续动画上限 30fps"和"后台掉到 2fps"，
  换成两条真实约束（某些分辨率 HWC 扫不出去；默认档位是拿"可能覆写"换流畅，
  并给出 `safe_present` 开关）；
- README 新增 **"通知里的配置面"** 整章（三个按钮 + 四条命令 + 灰字提示的含义），
  以及"怎么确认成功了"里补上 `mode` / `active` / `frames_pushed` 三个可量化判据；
- "另外两个运行时参数"那节把 `edid_dpi` 说清楚了：它只在合成器**启动时**生效，
  想要实时改 UI 大小要用通知里的 `scale`；
- 鸣谢里把 anland 写实：本项目的帧时序方案直接来自它的开发分支（tag 6.0–6.2），
  "帧完成要在 vsync 那一拍发"和"某些尺寸 HWC 扫不出去"两条都是它先量出来的；
- 删 `AUDIT.md`（早期审计残留，结论已被后续工作推翻）；删 `test/` 下那批编给
  别的 ABI 的二进制（在 ARM 设备上跑不起来），只留源码。

## 本版本第五十一项：换档后第一帧被自己清掉（"尾帧"的真正原因）

第五十项之后帧率上来了，但换档（切横竖 / 全面屏↔黑边屏）后画面还是停在上一档
—— 静态桌面尤其明显：换了档、`active` 也变了，屏幕上还贴着旧尺寸的图。

### 根因：`vdrm_frame_clear()` 把新档的第一帧扔了

一次 modeset 提交里的执行顺序是：

```
commit_modeset_disables → commit_planes(pipe_update，把新尺寸这帧推进队列)
                        → commit_modeset_enables(pipe_enable)
```

`pipe_enable` 里原来有一句 `vdrm_frame_clear()`（本意是"上一档遗留的帧不能留给
App，否则会按旧尺寸贴到新窗口"）。它把**刚刚推进去的、新尺寸的那一帧**一起清掉了。
静态桌面换完档不会再有新提交，于是 App 永远等不到帧事件，只能继续贴着旧帧 ——
这就是"尾帧"。有鼠标时之所以看起来时好时坏，是因为鼠标一动就又来了新提交。

### 修法（两处）

1. `pipe_enable` **不再清队列**，改成补推一次当前帧（`vdrm_push_current_frame()`）。
   旧尺寸的帧留着没害处：App 按 slot id 过滤，没 forge 过的 id 直接丢，而换档后
   它只会 forge 新尺寸的槽。
2. App 在"因尺寸变化重建池"成功之后补一发 `READY`。因为 `pipe_enable` 推那一帧
   时，App 往往**还没 forge 完**新尺寸的槽（会被当成 unknown slot 丢掉），
   rebuild 完再要一次才是真正接上。

### 实测（静态桌面，不注入任何输入）

```
竖：mode=1080x1920 active=1080x1920  →  截图：竖版桌面正确
横：mode=1920x1080 active=1920x1080  →  截图：横版桌面正确（画面填满，无竖条残留）
```

修之前同样的操作会停在上一档的 letterbox 画面上不动。

## 本版本第五十项：flip-done 回到"下一个 vblank"，不再等 SF 的 present（30fps → 88fps）

第四十五项把 flip-done 改成"等 SF 的 present fence"，第四十六项又给它加了存活兜底。
量下来端到端 30ms + 偶发 100ms 卡顿，一直以为那是"代理跳的物理代价"。**不是。**

### 证据（别人的实测）

anland 6.2（`services/waylandbridge/awl_sc.cpp` 头部设计注释）跟我们同构 ——
用户态造池、把 dma-buf forge 成 AHardwareBuffer、`setBuffer` 贴到 SurfaceControl
兄弟图层。它把结论写在最前面：

> frame_done goes out at the vsync TICK for every visible layer … **not from SF's
> OnComplete (one vsync later: a frame-callback-paced client then ran at 29 fps)**

也就是：**从 SF 的完成回调发"帧完成"，按 frame-callback 跑的合成器就只有 29fps。**
我们量到的 30fps、`present_lat≈30ms`、偶发 107ms，全是同一个坑 —— 把这条链里
最慢的一环（present fence）当成了节拍源。

### 改法

- `vdrm_pipe_update()` 不再把事件挂到池槽上，直接 `drm_crtc_arm_vblank_event()`
  到下一个 vblank（拿不到 vblank 引用才退回立即完成）；
- 删掉整条"挂事件"的机制：`pend_event` 字段、`vdrm_flip_complete()`、
  `pipe_disable`/`misc_release`/`UNREGISTER`/模块卸载里的四处排干、
  tick 里的存活超时、`VDRM_FLIP_FALLBACK_MS` 与 `flip_fallback_done`；
- `VDRM_IOCTL_PRESENTED` 降级成**纯诊断**（只记 commit→present 耗时，
  不再参与完成）；
- 槽的生命周期不变：App RETURN（SF 的 release）之前一直是 DISPL，
  `dumb_create` 不会把它发给别人。

### 实测（连续注入鼠标位移 10 秒）

| | 改之前 | 改之后 |
|---|---|---|
| 帧率 | 30 fps | **88 fps** |
| App 侧 2 秒事件数 | 51–60 | **169–171** |
| evgap > 80ms 次数 | 有（最长 128ms） | **0**（最长 40–59ms） |
| `latch_max` | 2–5ms | 2–5ms（没变） |

`present_lat` 从 30ms 降到 17.5ms —— 它现在只是"这一帧多久真的上屏"的观测值，
不再是节拍。

### 代价：`frames_reuse` 涨了，要盯着

改完之后合成器会在 SF 还没放手时就重提交同一块 fb：

```
鼠标移动 10s：889 帧，reuse Δ519
```

这正是当初挂住事件的动机。anland 的解法是让**客户端手里的 buffer** 带 SF 的
release fence 回池（`setBufferWithRelease` 的 OnRelease），客户端自然拿不到
正在显示的那块。我们的合成器（niri）不知道这层，它只认 flip-done。

所以这项改动是**用"可能覆写正在显示的显存"换 88fps**。验收标准：
- 肉眼无撕裂（录屏抓不到，只能看屏）；
- 如果看到"局部色块/半帧"，说明这条路要退 —— 那就退化成本版本之前的
  present 节拍（30fps），或者按 anland 的做法把 release fence 一路传到合成器侧
  （工程量大得多）。

## 本版本第四十九项：换档成功后自动回收旧档的整批槽（池不再无限累积）

上一项修好"非基础档不生效"之后，任意 WxH 都能切了 —— 于是暴露了池的容量问题：
**池是按档累积的**，每试一个新分辨率就多 6 块，而 `VDRM_POOL_MAX` 是 32，
也就是最多 5 档。试到第 6 档时 `mkpool` 就没地方建新槽，失败现象是
`dumb_create -ENOSPC` / 黑屏 —— 从现象根本看不出是"历史档把池占满了"。

（这也是"重建池是不是冗余"那个疑问的正确答案：**回收策略没问题，容量管理缺了。**
旧档的槽在换档之后就没有意义了，应该自动扔掉，而不是留个按钮让人手动点。）

### 修法

新增 `vdrm_pool_drop_stale()`：把"**尺寸不是当前 mode 参数**、**不在 DISPL**、
**合成器已经不再持有**"的槽整块回收（`fput` f0/f1、`vdrm_gem_put obj`、清表项）。
三条安全条件缺一不可：DISPL 的槽可能还在屏上，必须等 App RETURN；
合成器还攥着 handle 的槽不能抽，否则等于把它手上的显存从池里拿走。

两个调用点：

- `vdrm_dumb_create()` 开头 —— 进程上下文，可以睡眠着放引用；而且每次换档
  合成器一定会先来这里要新尺寸的 buffer，正好是清理时机；
- `vdrm_pipe_enable()` 换档完成处 —— 这时旧档的 buffer 一般已经被合成器销毁。

### 实测（四档来回切，池不再涨）

```
3.8 之前：试 4 档 → 24 槽（1920x1080/1080x2376/2376x1080/1080x1920 各一档）
3.8 之后：2247x1264 → pool 8    2780x1264 → pool 9
          1080x1920 → pool 8    1920x1080 → pool 8
          dmesg: dropped 4+1 stale slot(s) (keep 1080x1920) …
```

池稳定在 8 槽左右（当前档 6 + 正在退场的 1~2 块），不再随换档次数线性增长，
所以"重建池"按钮可以真正去掉了（`mkpool --reset` 仍保留给异常场景）。

## 本版本第四十八项：修 `mode` 改到"非基础档"时不生效（对外广告列表漏了当前档）

症状：`echo 1080x2376 > mode` + `mkpool --mode 1080x2376` + `commit` 全部返回成功，
`mode` 参数、`mode_slots`、池里的槽、App 的转屏全都对了，**但屏幕还是原来的档**。
dmesg 里能看到的证据是：`get_modes -> 12 (4 个分辨率 … preferred 1080x2376@120)`
之后**没有 `pipe enable`** —— 合成器压根没做 modeset。

### 根因

`g_modes[]`（对外广告的列表）只装三样东西：基础两档 `1080x1920` / `1920x1080`、
App 上报的窗口尺寸横竖两档。**当前 `mode` 参数那一档从来不在里面**，除非它恰好
等于上面四者之一。

而 `vdrm_conn_get_modes()` 里那句 PREFERRED 是这么标的：

```c
if (w == g_mode_w && h == g_mode_h && r == 0)
        m->type |= VDRM_MODE_TYPE_PREFERRED;
```

列表里没有这一档 → **一个 PREFERRED 都没有** → 合成器热插拔后没有目标可挑，
原地不动。所以"参数全对、屏幕不变"。基础两档（`1080x1920`/`1920x1080`）一直是好的，
这就是为什么这个问题只在试别的分辨率（比如 `1080x2376`、`2376x1080`）时暴露。

顺带一个陷阱：那行日志里的 `preferred %ux%u` 打的是 `g_mode_w/h` 本身，
**不管它在不在列表里** —— 日志说 preferred 1080x2376，列表里却没有它。
以后看这行要配合 `cat modes` 一起看。

### 修法

- `vdrm_modes_rebuild()` 末尾把当前 `mode` 参数那一档并入列表（去重、受
  `VDRM_MAX_MODES` 限制）；
- `VDRM_MAX_MODES` 4 → 6（基础 2 + 屏幕 2 + 当前档 1 = 5，留 1 个余量；
  池 32 槽 / 每档 6 个，正好装得下）；
- `vdrm_mode_apply()` 里调一次 `vdrm_modes_rebuild()`，**prepare 阶段**就把列表
  更新好（commit 时的 hotplug 直接带着它）。开机路径同样走这里，所以
  `insmod mode=WxH` 也不再依赖"恰好是基础档"。

### 实测

```
# 修之前
commit -> 1080x2376 …  get_modes -> 12 (4 个分辨率)   ← 没有 pipe enable，屏不动

# 修之后
$ vdrm_mkpool --mode 1080x2376
modes=1080x1920,1920x1080,1264x2780,2780x1264,1080x2376
get_modes -> 15 (5 个分辨率 x 3 档刷新率, preferred 1080x2376@120)
pipe enable 1080x2376 period=8333337 ns          ← 合成器真的换了
niri: Current mode: 1080x2376 @ 120.000 Hz (preferred), Logical size 480x1056

$ vdrm_mkpool --mode 2376x1080     → pipe enable 2376x1080
$ vdrm_mkpool --mode 1920x1080     → modes 回到 4 档（当前档与基础档重合），pipe enable 1920x1080
```

所以现在 `mode` 命令对任意 16..4096 的 WxH 都成立，不再局限于那两档基础分辨率。

## 本版本第四十七项：把端到端的 30ms 拆开量（纯诊断）

第四十五、四十六项之后，链路上只剩一个数：端到端 **30ms**（`present_lat_us`），
合成器串行 → 30fps。但"30ms 花在哪"一直靠猜。这一项把它拆成两段来量。

### 怎么拆

`ASurfaceTransactionStats_getLatchTime()` 给出"这一帧被 SF 锁定"的时刻，
于是 App 侧能算出两段：

```
apply -> latch    App 提交事务 → SF 锁定它          （唯一我们能动的一段）
latch -> present  SF 锁定 → present fence signal     （SF 合成 + 扫描出）
```

第三段（flip-done → 合成器下一帧 commit）已经由内核的 `present_lat_us` 和
`fence_*` 覆盖，不重复量。实现就是按槽记两个时间戳 + 四个 u64，随 2 秒
一次的心跳打到 logcat：

```
STATS events=… shown=… | apply2latch=7092us(max 18833) latch2present=13192us(max 104972)
```

### 实测：它是**双稳态**的

| 状态 | apply→latch | latch→present | 合计 | 帧率 |
|---|---|---|---|---|
| A | 7ms（0.85 vsync） | 13ms | 20ms | **45 fps** |
| B | 14ms（1.7 vsync） | 15.5ms | 30ms | **30 fps** |

同一个负载（glmark2）下两种状态会各自锁住一段时间再切换。区别只在
**apply 有没有赶上 SF 这一拍的 latch**：赶上就是 0.85 个 vsync，赶不上就是
1.7 个。这也解释了之前几轮测出来 30/31/33fps 的抖动。

### 试过但无效的

`ASurfaceTransaction_setDesiredPresentTime(now)`（"马上送我"）：a2l 仍是
14ms、帧率仍是 30fps，SF 不按这个 hint 提前。已删掉，不留每帧一次的无用调用。

### 顺手排除掉的一个方向

`dumpsys SurfaceFlinger` 里我们那层：

```
Layer [5045] vdrm#5045
  blend=NONE (1) … composition type=DEVICE (2)
```

`DEVICE` = HWC overlay，**不是 GPU 合成**。所以"再省一次 GPU 合成"这条路
不存在 —— 这一层已经在硬件叠加平面上了。

### 结论：这 30ms 里没有我们的活了

两段各约 2 个 vsync，都是 SF 内部的 latch + 合成/扫描深度。App 侧的
"收事件 → apply"经核算只有 1ms 以内（内核 `present_lat` 28.8ms vs App 侧
两段合计 30ms，差就是它 + 测量噪声）。想再快只能动架构（去掉这层代理，
或者让合成器不再串行），不是调参。

## 本版本第四十六项：兜底 100→500ms（它该判"死没死"，不该管延迟）+ READY

第四十五项把 flip-done 改成"只认 present fence"。这一项收拾它剩下的两处：
兜底超时的**取值**、以及"回前台"这条恢复路径。

### 1. `VDRM_FLIP_FALLBACK_MS`：100 → 500

100ms 一直在干两件互相冲突的事：既是"App 死了"的存活检测，又事实上成了
**延迟上限**。而实测（第四十五项）满负载下 `present_lat_max` 是 88ms ——
离 100ms 只差 12ms。负载再重一点、或者换个慢一点的 SoC，兜底就会在
**健康帧**上提前触发，把还没上屏的帧判成完成，正好制造它本来要防的撕裂。

改成 500ms，职责收窄成"存活检测"：

- 正常前台：压根不会触发（实测满负载 `flip_fallback_done` 恒 0）；
- App 在后台：present 不来，桌面按 1/500ms ≈ 2fps 跑。反正没人看，
  比之前 10fps 更省电；
- 代价是"回前台"要显式叫一声，见第 3 点。

### 2. 计数器拆开 + 删掉 tick 里那条死分支

`flip_present_done` 以前把"present 完成"和"兜底完成"记在同一个数里 ——
这样没法验证兜底有没有误触发。拆成：

```sh
flip_present_done     # 正常路径
flip_fallback_done    # 兜底触发；正常应该是 0
```

有了它，第 1 点那个取值才有验收标准（满负载跑一遍，`flip_fallback_done` 保持 0）。

同时删掉 tick 里"present 之后已经过一个周期就完成"的分支：present 改成就地
完成之后它已经走不到了，而且会拿上一帧留下的时间戳误判新事件。那个时间戳
字段（`presented_at_ns`）本身也随之变成只写不读，一起删掉 —— 和之前删
`displ_at_ns` 是同一类清理。

### 3. 新增 `VDRM_IOCTL_READY`：回前台时主动推一次当前帧

后台期间 present 不来，合成器的下一次提交可能还要等一阵；回前台如果干等，
屏幕会先停一下再"醒"过来。READY 做三件事：

1. `vdrm_push_current_frame()` —— 把当前在屏的那一帧**立刻**重推给 App，
   不等合成器提交；
2. 预热 3 拍 vblank + signal App 的 eventfd；
3. App 侧同时把缓存的最后一帧重贴回图层（`sc_show_last()`）。

调用点：Java `MainActivity.onResume()` → `Native.nativeSetReady()`。
另有一条已经存在的隐式路径：`nativeStart()` 会重新注册 eventfd，内核那边
"第一次接上就补推当前帧"的分支同样会命中 —— 所以 surface 被重建的那条路
本来就已经有"就绪"效果，READY 补的是 surface 没变、只是重新可见的那条路。

`GET_INFO.flags` 新增 `VDRM_CAP_READY`（bit 9），老 App 忽略即可；
老模块没有这个 ioctl，App 侧吞掉 ENOTTY。

### 实测（glmark2 满负载，120Hz 输出）

| 场景 | 结果 |
|---|---|
| 前台稳态 | 30 fps，`present_done=231`、**`fallback_done=0`**、`reuse=0` |
| 压到后台 | 2 fps，`fallback_done` 每帧 +1（纯兜底，符合设计） |
| 回前台第 1 秒 | 36 帧（已经是满速，没有"卡一下"） |
| READY（用测试小工具直接打 ioctl） | dmesg: `app ready -> push current frame` |

注意：后台阶段 `frames_reuse` 会缓慢增长 —— 那是"合成器重新提交了一块 App
还没 RETURN 的槽"，在没人看的时候无害；**这项指标只在"前台"下才有意义**。

## 本版本第四十五项：flip-done 只认真实上屏信号（SF present fence），RETURN 退回只管回收

第四十二项把 flip-done 挂到槽上，交给"App RETURN / present 后一个 vblank / 100ms
兜底"三条路完成。这一项把其中两条收掉，只留精确的那一条。

### 1. App 报的 present 改成等 SF 的 present fence

以前 `setOnComplete` 一到就报 `PRESENTED`，然后**内核再猜着多等一个完整周期**
才敢完成 flip-done（怕 DPU 还没扫完）。猜的那一周期是纯延迟。

`ASurfaceTransactionStats_getPresentFenceFd()`（API 29+）返回的是"这一帧真的
上屏了"的 fence，所以：

- fence 有效 → App 把它塞进 `release worker` 的 poll 集合，signal 了才报
  `PRESENTED`（不新起线程）；
- fence 是 -1（部分设备不支持）→ 立刻报，行为和以前一模一样。

设备实测这颗 SoC 是支持的（`fence=169` 这种），所以走的是精确路径。

### 2. 内核：present 一并就地完成，不再等下一个 vblank tick

既然 present 报的是"fence 已 signal = 已经在屏上"，就没有理由再等一拍。
`VDRM_IOCTL_PRESENTED` 里直接把槽上挂的事件完成掉。定时器那条路只剩兜底
（present 不来 / App 挂掉）。

### 3. RETURN 不再兼职完成 flip-done

RETURN 的语义只有一条：SF 放手了，这块显存可以再分配。以前它还顺手完成
flip-done，等于让"SF 什么时候释放"兼职"什么时候上屏" —— 两条路的慢的那条
会把合成器一起拖住。分开之后少一条完成路径。

### 4. 删掉 `flip_done_immediate`

它是个"一键退回撕裂"的调试开关（完成不等上屏，合成器立刻覆写正在扫描的
显存）。正确路径已经实测通过，留着只有害处。

### 实测：省下来的不是性能，是猜测

同一个负载（glmark2 经 Xwayland，客户端 400fps，120Hz 输出，App 在前台，
各 8 秒窗口）：

| 版本 | frames_pushed | 每帧耗 vblank |
|---|---|---|
| 旧（三条路 + 猜一个周期） | 33 fps | 3.62 |
| `flip_done_immediate=1`（旧） | 87 fps | 1.00 |
| 本版（present fence + 就地完成） | 30 fps | 3.43–3.86（跑间抖动） |

**没变快。** 因为瓶颈不在我们猜的那个周期上：新加的 `present_lat_us` 直接给出
了答案 —— 从合成器提交到 App 报 present 要 **31ms**（3.7 个周期），也就是
"App 图层 → SF 合成 → 扫描输出"这一趟本身。合成器是串行的（上一帧
flip-done 才开始画下一帧），所以帧率 = 1/31ms ≈ 32fps，和实测的 30 对得上。

所以这一项的实际收益是：**完成时机从猜变成已知**（以前可能偏早、也可能偏晚），
少一条完成路径，少一个危险开关，外加一个能解释"为什么只有 30fps"的诊断。
想真的提速只能动架构（削掉代理跳），不是调这层同步。

### 新增诊断

```sh
cat /sys/module/vdrm/parameters/present_lat_us      # 最近一次 commit -> present（us）
cat /sys/module/vdrm/parameters/present_lat_max_us  # 历史最大值
```

`frames_reuse` 仍是 0，说明"合成器覆写 App 还没归还的槽"这条撕裂路径没被触发。

## 本版本第四十四项：把"配置面"收成一个形状 —— 三参数共用 prepare/commit + 能力位图

前面四十三项解决的都是"跑不跑得对"。这一项解决的是**这些料怎么摆**：模块只保留
机制，配置全部下沉到用户态，上游只承诺语义和 ABI。

### 1. mode / refresh / edid_dpi 三个参数统一成一件事

之前只有 `mode` 支持运行时改，`refresh` 和 `edid_dpi` 是 0444 —— 想改刷新率或
UI 缩放只能 reload 模块，也就是**必须重新刷 .ko 才能改一个显示参数**。而对锁了
bootloader 的机器，那是最贵的一次操作。

改法：三者都进同一套两段式。

- 写参数 = 只"准备"：记值、置 `g_cfg_pending` 的对应位（`VDRM_CFG_MODE` /
  `VDRM_CFG_REFRESH` / `VDRM_CFG_DPI`），不惊动合成器；
- 写 `commit` = 一次性生效：重算 EDID + hotplug + 预热 3 拍 vblank。

三者都进 EDID（refresh 进 detailed timing 的 vtotal 和 0xFD 的 vrate range，
dpi 进 mm），所以合并进同一个时间点是**语义上必须**的，不是图省事：合成器什么时候
重读 EDID 不可控，就地改会出现"参数改了屏幕没变"或者"尺寸新、刷新率旧"的半生效态。

`g_mode_pending`（bool）随之换成 `g_cfg_pending`（位图）。

一个细节：`commit` 里只有 `VDRM_CFG_MODE` 置位时才 `vdrm_frame_clear()`。之前
无条件清，而 refresh/dpi 不改几何 —— 那条尾帧是合法的，清掉会让合成器等一个
永远不来的 flip-done（表现为换刷新率后卡一下）。

### 2. GET_INFO.flags 从"保留"变成能力位图，ABI 1 → 2

用户态**不要拿版本号比大小**，要问"你支持这个吗"。老模块 `flags == 0`，含义是
"这版没有能力位图"，App 按 ABI 1 的老行为跑；新模块自己报位。这样 App 可以比
模块新，也可以比模块旧，四种组合都不互相掐死。

结构布局没变（`vdrm_info` 还是 24 字节），所以不需要新 ioctl 号，也没有 ABI 断裂。
位只增不改：分配出去就不复用，删能力靠不再置位。当前 9 位：
`MODE_RUNTIME` / `REFRESH_RUNTIME` / `DPI_RUNTIME` / `EVENTFD` / `PRESENTED` /
`POOL` / `INPUT_BATCH` / `TOUCH` / `POOL_HANDLE_GONE`。

App 侧 `vdrm_open()` 会打一行 `abi=2 caps=0x1ff [mode,refresh,dpi,...]` ——
出问题时先看这行就知道手上是哪个模块、支持什么面。

### 3. 顺手清掉的债

- `common.sh` 里每次 insmod 后 `echo 0 > flip_done_immediate` 是冗余的：
  内核默认就是 `false`，脚本里那句注释（"默认 Y"）反而是错的。删掉，
  少一个参数、少一处可能忘。
- `mode.conf` 增加 `refresh=` 开机种子（24..240，越界只记日志不加载）。
- 过时注释：`native_consumer.c` 里"mkpool 由 root watcher 补池"（watcher 已删）、
  `/dev/vdrm` 权限注释还写着"service.sh chown 到 App uid 0600"（实际是
  `vdrm_fix_nodes` 的 0666 + gpu_device 标签，故意不 chown）。注释和实现反着说
  比没注释更贵，一起改掉。

## 本版本第四十三项：摘掉常驻 watcher，清理一批冗余设计

- **池补齐**：`vdrm_mkpool --mode` 已经走完 prepare → 建槽 → commit；槽回收修好后
  来回切也不会耗尽。watcher 里 `mode_slots < 4` 就补池的分支因此是冗余的。
- **输入隐藏**：`post-fs-data.sh` / `service.sh` 已经在模块加载后一次性
  `vdrm_hide_input`（重试窗口 5s → 15s）；`tools/vdrm_up.sh --reload` 也会补一刀。
  删掉 `vdrm_watch.sh`、`vdrm_ctl`/`vdrm_ctlwait`、`vdrm_hide_watch_start`，
  以及内核的 `SET_CTLFD` / `g_ctl_ctx` / `vdrm_ctl_signal`。
- **内核死字段**：压力回收删掉后 `displ_at_ns` 只写不读，删除。
- **App 死代码**：`alloc_buffers` / `vdrm_register` / 旧 `vdrm_register_req` ABI、
  `anb_dma_buf_fd`、`anw_hidden.h`、`keymap_us.h`、`buf_info.dma_buf_fd`、
  KPatch 时代的 `VDR2_CTL0_CMD` / `VDR2_KPATCH_PATH` 宏全部删除。
  App 现在只从池 forge，不再保留第二套“自己分配再 REGISTER”的路径。

## 本版本第四十二项：槽回收改成分配生命周期 + 换档尾帧收尾

### 1. 槽回收：从“会话内一次性”改成“handle 销毁即释放”

第一项把归还推迟到合成器 `postclose`，本意是防别名，但副作用是**一个槽在
整场合成器会话里只能发一次**。合成器每换一次分辨率都会销毁旧 buffer、重新
`CREATE_DUMB`；每档只有 6 个槽，来回切两三次就耗尽，`dumb_create` 空等 6 秒
后 `-ENOSPC`（niri: `Failed to create dumb buffer / No space left on device`）。
`mode_slots` 又只数 `used + 尺寸`，不数 `allocation_reserved`，watcher/mkpool
因此以为池是满的，不会补。旧的“压力回收”只把 `state` 置 FREE，`reserved` 还在，
既救不了分配，又把 `pend_event` 留在槽上 —— 下一次 `pipe_update` 直接覆盖事件，
旧 flip-done 永远不发。

本版改法：

- 实现 `dumb_destroy`：合成器销毁 dumb buffer（gbm 路径必经）时释放分配占用；
  按 `obj` 匹配，且 `handle_count <= 1`（最后一个 handle）才放；
- DISPL 槽只记 `handle_gone`，等 App `RETURN` 再放 —— 那块可能还在扫描输出，
  提前放开会让下一个 BO 覆写同一片内存（别名）；
- `postclose` / `GEM_CLOSE` 兜底：`postclose` 对非 DISPL 槽直接放，DISPL 槽
  留到 `RETURN`；`dumb_create` 选槽前对 `handle_count == 0` 的残留槽懒回收；
- 删除 pressure reclaim、`displ_grace_ms`、`frames_stale_reclaim`。

### 2. 换档尾帧：关 CRTC 先收 flip-done，尾帧不推给 App

- `vdrm_pipe_disable()` 先把所有 `pend_event` 完成（它们被从 `crtc_state` 摘走，
  不在 `dev->vblank_event_list` 里，`drm_crtc_vblank_off()` 不会替我们发），
  再停网格；
- vblank 预热 3 拍到期不再直接停网格：统一由 `!left && !g_vblank_on && !kick`
  判定，避免换档时把还挂着的旧事件连同 100ms 兜底一起停掉；
- `pipe_update` 在 `mode_changed && fb 没变 && fb 尺寸 != 新 mode` 时判定为
  尾帧：不推帧、不把事件挂到旧槽；覆盖已有 `pend_event` 前先补发；
- `commit` / `pipe_enable` / `pipe_disable` 清 `g_frame_latest`；
- App：每 200ms 查一次 `GET_INFO`（不再依赖 poll 超时）；未知 slot 先对账再
  重 forge；去掉“以帧为准改 `c->mode`”的回滚路径，尺寸不符直接丢并 RETURN；
  重建池不再同步等 10 秒，也不提前 `sc_blank`（旧帧按自身尺寸 letterbox 显示，
  直到新尺寸第一帧到达）。

## 本版本第一项改动：池槽的会话边界归还

驱动维护一个由 App 注册的显示缓冲池，合成器通过 `CREATE_DUMB` 从池里取缓冲。此前版本有一个缺陷：

- `RETURN`（App 显示完毕）会把槽置为 FREE，但合成器仍持有该缓冲的 GEM handle／导出 fd／GPU 导入；
- 下一次 `CREATE_DUMB` 会把同一物理内存再发一次 → 两个合成器 BO 指向同一片内存（别名），表现为启动阶段卡顿、局部画面错乱。

本版本的处理：

1. 分配成功后该槽记 `allocation_reserved`，**会话内不再发放**（消除别名）；
2. 归还**只发生在合成器的 drm_file 关闭时**（`drm_driver.postclose`）：解除占用，TAKEN 的置 FREE，DISPL 的仍由 App 的 `RETURN` 决定；
3. 不再用 `handle_count == 0` 推断占用结束，也不在池满时回退非池缓冲（改为返回 `-ENOSPC`）。

效果（已在参考设备实测）：首次启动流畅；合成器退出后再次启动能重新取到缓冲，可反复启停；分配日志中同一槽跨会话复用时 `handle_count` 均为 0，无别名。

## 本版本第二项改动：`atomic_check` 不再阻塞提交

上一版的 `vdrm_atomic_check` 在发现本次提交要用的池缓冲仍处于「已上屏、App 未 `RETURN`」时，会这样做：

```c
while (g_pool[idx].state == VDRM_ST_DISPL && w < 50) {   /* 最多 50 次 */
        msleep(1);
        w++;
}
```

这是个**自锁的等待**。App 的 `RETURN` 挂在 Android 的 release fence 上，而 SF 只有在收到更新的 buffer 之后才会释放旧 buffer；更新的那一帧由本次提交的 `pipe_update` 推出。串起来是：

```text
gate 等 RETURN(N)
      ← SF 释放 N（release fence 触发）
      ← App 提交 N+1
      ← App 收到帧事件 N+1
      ← pipe_update(N+1) 推事件
      ← 本次提交完成 —— 而它正被 gate 挡着
```

也就是说，它等的事情只有它先放行才可能发生，必然空等到超时。

实测（HZ=120 时 `msleep(1)` 实际睡满一个 jiffy ≈ 8.33ms，50 轮 ≈ 420ms）：日志里**每一次都跑满 50 轮才放行，没有一次提前退出**。

后果：合成器每做一次包含池缓冲的提交就被冻结约 420ms。拖拽鼠标时每次移动都触发一次提交，于是光标只能每约 0.5 秒跳一格 —— 看上去「只显示起点和终点」，同时桌面整体帧率塌到约 1/3。

**本版去掉这个等待**，只保留计数。同一台设备、同一段拖拽实测：

| 指标 | 改动前 | 改动后 |
|---|---|---|
| 提交间隔 p50 | 31.8 ms | 31.8 ms |
| 提交间隔 p99 | **465 ms** | **34.7 ms** |
| 拖拽中 >150ms 的停顿 | **46 次** | 9 次（基本落在起手/收手边界） |

基础节奏（约 31ms，由 App 渲染回路决定）两边完全一致 —— 差别全在那几十次 420ms 冻结。

去掉它不损失语义：该缓冲本来就会在 `pipe_update` 里被重新置 `DISPL` 并补推帧事件，提交路径不需要在此阻断。

## 本版本第三项改动：vblank 网格改为连续

虚拟显示没有真实扫描，vblank 是驱动用 hrtimer 造的。旧写法有两个问题：

```c
/* enable_vblank：每次取引用都从“现在”重新起算一整个周期 */
g_vblank_on = 1;
hrtimer_start_range_ns(&g_vblank_timer, g_vblank_period_ns, 0, HRTIMER_MODE_REL);

/* disable_vblank：放引用时把它取消 */
g_vblank_on = 0;
hrtimer_try_to_cancel(&g_vblank_timer);
```

合成器每提交一帧都走 `get → arm → put` 一轮（日志里 `vblank on` / `vblank off`
各 1055 条，和 1055 次提交一一对应）。所以**每一次提交都要白等整整一个周期
（60Hz 下 16.67ms）才收到 flip-done** —— 不管它落在 vblank 周期里的哪个位置。
真实显示器的下一个 vblank 在 `[0, 16.67)` 内任意位置，我们这里永远是满值。

实测：提交间隔**恒为 31.8ms**（`16.67 等待 + ~15 渲染`），纹丝不动。表现就是
拖动鼠标时每 31.8ms 光标才落一个新位置，看着有残影。

修法：vblank 网格**连续**。

- `enable_vblank` 只在网格没跑时启动，之后不再重起（相位不被打断）；
- `disable_vblank` 只放引用，**不**停定时器；
- 定时器回调持续自转发；没人持引用时不调 `drm_crtc_handle_vblank()`
  （此时 `vblank->enabled` 为 false，调了会 WARN）；
- 网格由 `pipe disable`（`vdrm_pipe_disable`）和模块卸载统一停。

不引入任何新的内核符号（`hrtimer_start_range_ns` / `hrtimer_forward` /
`hrtimer_try_to_cancel` / `drm_crtc_handle_vblank` 本来就在引用表里），
所以 CRC 采集结果不用重做。

效果：参考设备上拖拽变顺滑（使用者确认）。另外留了一个诊断旋钮：

```c
#define VDRM_VBLANK_HZ_FORCE 0   /* 非 0 时强制虚拟显示的帧率(Hz)，0 = 按 mode 反推 */
```

用来分辨“合成器跟不上”到底是 vblank 定拍造成的，还是它自己渲染慢：
把它设成 30 或 120 重编一次，看提交间隔跟着变不变即可。默认 0，不改变行为。

## 本版本第四项改动：虚拟键鼠改成内核里的 input 设备

早前版本里，虚拟键鼠是 App 用 `socketpair` fork 一个 root 守护进程（`vdrm_inputd`），
由它打开 `/dev/uinput` 造设备、再把事件写进去。

这个结构有个致命问题：**设备属于那个用户态进程**。App 一退出守护进程随之退出，内核销毁
设备；App 再进来设备被**重新创建**。而容器里的合成器**没有任何办法知道设备换过了** ——
容器 udev 收不到宿主 netns 发出的 uevent（见下节），一旦设备被销毁，libinput 会永远抱着
那个已经死掉的 fd。表现为"App 重启一次，输入永久失效"。

本版本把设备搬进驱动：由 KO 直接注册两个 `input_dev`，事件经 `VDRM_IOCTL_INPUT` 注入。

```
App ──ioctl(/dev/vdrm, VDRM_IOCTL_INPUT, {type,code,value})──► KO ──input_event()──► evdev
```

设备生命周期从此**绑在模块上**，App 反复开关都不动它。顺带两个好处：

- **`vdrm_inputd` 整个消失** —— 少一个进程、少一条部署路径
- **节点由内核 / ueventd 管** —— 注册时建、注销时删，不会留下悬空节点

亚像素累积与滚轮折算从守护进程上移进了 App。新增的内核引用 6 个（采集时会自动校验）：

```
input_allocate_device  input_free_device   input_register_device
input_unregister_device  input_event       input_set_capability
```

> `input_sync` 在本内核里**没有导出符号**，所以同步改用
> `input_event(dev, EV_SYN, SYN_REPORT, 0)`。

设备名固定为 `vdrm-kbd` / `vdrm-ptr`。它们仍然是**内核级**输入设备，Android 侧也会认到，
所以 `/vendor/etc/excluded-input-devices.xml` 里那两条**仍然是必需的**。

## 本版本第五项改动：音频真正能出声

音频刚接上时有几个会把声音彻底掐死的问题，逐个记一下。

### 1. mmap 长度没按页对齐（直接原因）

共享环是 `sizeof(header) + 512KB` = **524320** 字节，不是页的整数倍；而 mmap 的长度
会被内核向上取整到 **528384**。`vdrm_audio_mmap()` 用未对齐的 `total` 做上限检查，
合法映射直接被 `-EINVAL` 拒掉 —— App mmap 不上，音频根本起不来。
改成 `PAGE_ALIGN()`，alloc / free / mmap 三处用同一个值。

### 2. `ack` 里睡眠 / `period_elapsed` 自锁

`ack` 是在 ALSA 流锁里被调用的（`pcm_lib_apply_appl_ptr()` 位于
`snd_pcm_stream_lock_irq()` 之后）：

- 旧的 `.copy` 路径在环满时 `wait_event_interruptible()` —— 睡在自旋锁上；
- 在 `ack` 里调 `snd_pcm_period_elapsed()` —— 同一把非递归锁重入，自锁。

现在 `ack` 只做非阻塞搬运，环满就丢尾巴并记账（`dropped_bytes`），
绝不在内核里等 App。

### 3. 阻塞写没人叫醒，10 秒后 -EIO

ALSA 的阻塞写在没空间时睡在 `runtime->tsleep` 上，**默认只等 10 秒**，超时直接
返回 `-EIO`；它不会自己轮询 hw_ptr。所以光把 `pointer` 改成按 `tail` 回报还不够：
必须有人推 `period_elapsed`，否则写者永远不会被唤醒。

- App 每消费一段（约 20ms）就 `ioctl(VDRM_AUDIO_KICK)`；
- 内核在锁外调 `snd_pcm_period_elapsed()`，更新 hw_ptr 并叫醒写者；
- App 关闭（`release`）时也踢一次，让写者立刻走"没人收、直接丢"的退化路径，
  不用干等 10 秒。

> 这是新增 ABI：`VDRM_AUDIO_KICK = _IO('v', 0x31)`。
> 新 .ko 必须配新 APK，旧 APK 不会踢，阻塞写会周期性 -EIO。

### 4. `pointer` 零延迟 + `ack` 一次前进整 buffer 丢数据

- `pointer` 原来直接返回 `appl_ptr`，等于告诉 ALSA"写进来就已经播完"：播放器
  满速灌满 512KB 环，然后被丢帧，延迟越来越大、音画对不上。现在按 App 的
  `tail` 增量算真实消费；App 没开时退化成 `appl_ptr` 并丢弃新数据。
- `ack` 的增量原来写成 `(cur - last + frames) % frames`。一次 ack 正好前进一个
  buffer 是合法的（prepare 后写满整块），取模会得到 0，且不更新 `g_last_appl`，
  之后每次都看到 `frames` 的整数倍 → 永久静音。改成未取模的差，防御性钳到
  `frames`。

### App 侧

- 只按 `AAudioStream_write()` **实际写入的帧**推进 `tail`，超时/短写不再把没写
  进去的数据记成已消费；`AAUDIO_ERROR_TIMEOUT` 当背压重试，不当流已死；
- 一次最多搬 ~20ms，消费进度和唤醒都更细；
- `prepare` 与本线程 in-flight 写撞上时，`avail` 会回绕成巨大值；看到
  `avail > size` 就按重同步处理（`tail = head`）；
- `pthread_create` 失败时清掉已赋值的 `audio_fd/audio_map`，避免 `audio_stop`
  二次 `munmap`/`close`。

> 顺带：`struct snd_pcm_ops.ack` 的签名从 2.6.32 到 6.15 一直是
> `int (*ack)(struct snd_pcm_substream *)`，**没有** `appl_ptr` 参数；
> 按 6.1 分叉出两个签名反而编不过 5.10。`vm_flags_set()` 是 6.3 才有的，
> 这里继续用 `vma->vm_flags |=`。

诊断（只增不减）：

```sh
cat /sys/module/vdrm/parameters/dropped_bytes     # 环满丢的字节
cat /sys/module/vdrm/parameters/discarded_bytes   # App 没开时丢的字节
cat /sys/module/vdrm/parameters/ring_used_bytes   # 当前环里积压的字节
```

## 本版本第六项改动：延迟、播放/停止边界和 mixer

- ALSA buffer 从 512KB 压到 **32KB**（48k 立体声约 171ms），避免播放器把整条
  512KB 共享环当 ALSA buffer 灌满，造成 1 秒以上的音画延迟；
- 共享环加 **96KB 上限**（约 0.5 秒）：超过就丢新帧，不让启动时的积压一直留着；
- `prepare` / STOP / close 时清空共享环并抬 **generation**，App 看到代次变化就
  丢弃本地旧 tail；关闭视频后不会再把已经排队的尾巴放完；
- 注册 `Master/PCM Playback Volume`（带 TLV/dB）和 `Master Playback Switch`，
  给 ACP/WirePlumber 识别；驱动内部不再用这些控件衰减数据，避免 server 把
  音量设 0 时整条数据被乘静音；
- `snd_jack_new("Speaker")` 上报常插耳机口，给 ACP 建 Speaker 端口；
- 共享环 ABI 的 `reserved` 字段改为 `generation`（结构大小不变）。

## 本版本第七项改动：内核侧的锁、边界与生命周期

这一轮没加功能，只把"约定成立"改成"构造成立"。三块：

### 1. 池：一把锁、不在锁里睡眠、失败要回滚

旧版用 `g_pool_lock`（mutex）串行化池操作，但提交路径（`pipe_update`，原子上下文）
只能用 `g_pool_state_lock`，两把锁各自改 `state`/`allocation_reserved` —— 竞态是
结构性的，不是漏加锁。

- 池槽的**所有**字段（含 `f0`/`f1`/`obj`）现在只由 `g_pool_state_lock` 保护；
  mutex 删掉；
- 需要睡眠的操作（`drm_gem_prime_import`、`fget`、`get_unused_fd_flags`）一律移到
  锁外：先在锁内 `get_file()` 拿引用，放锁再装 fd。`GET_POOL` 原来是"锁内
  `fd_install` + 全局 `g_pool_list`"，现在改为**按调用分配池表 + 锁内快照 + 锁外安装**
  （顺带修掉两个进程同时 `GET_POOL` 互相踩全局表的问题）；
- `REGISTER` 原来"锁内选槽 → 放锁 import → 回来直接 memset 该槽"；import 期间槽可能
  被别人占走。现在 import 完**重新选槽**，并在同一临界区里"填字段、最后发布 used"；
- `dumb_create` 在锁内**预占**槽（`state=TAKEN`、`allocation_reserved=1`），
  `drm_gem_handle_create()` 放锁调用；失败回滚预占；
- `UNREGISTER`（含 id=0 清整池）与卸载：锁内置 `used=0` 摘出引用，`fput`/`gem_put`
  放锁外 —— 之前是锁外 `memset` 整个条目，`pipe_update` 可能读到半清空的指针；
- `pipe_update` 改为**先读 `used` 再读 `obj`**（同锁内），配合上面的"先置 used=0"，
  提交路径不会再看到正在释放的记录。

### 2. 卸载顺序：先断入口、停时钟、排干提交，最后回收池

旧顺序是"清池 → 停 vblank → `drm_dev_unregister`"，`pipe_update` 完全可能踩着正在
释放的池项。现在：

```
misc_deregister(/dev/vdrm)      /* 新 ioctl 进不来 */
hrtimer_cancel(vblank)          /* 提交路径里唯一还在原子上下文碰池的东西 */
drm_atomic_helper_shutdown()    /* 排干在飞的 atomic commit */
drm_dev_unregister()
清池（同一把锁、锁外释放）
drm_dev_put() / platform_device_unregister()
```

### 3. 初始化失败要回滚

`vdrm_init()` 的 `err:` 原来只打印一行就返回，已注册的 platform_device / drm_dev /
misc / 三个 input 设备 / 声卡全留在内核里，而模块加载失败会被卸载 —— 下次 insmod
撞资源。现在 `err:` 按相反顺序调用 `vdrm_audio_exit()` / `vdrm_input_teardown()` /
`vdrm_do_unregister()`（三者都能处理"只初始化了一半"的状态）。

## 本版本第八项改动：音频线的边界与 ALSA 契约

同一条音频线，这一轮修的是"正确性靠约定"的地方：

- **环头是用户态可写的**（整块共享页 mmap 给 App，含 `magic/size/rate/...`）。
  旧版 `.mmap` 用 `g_ring->size` 算映射长度、`ring_push` 用 `g_ring->size` 算
  `off = head % size` 和可写长度 —— 把 `size` 改大就能让内核从环的 PFN 起映射
  任意长物理内存、并越过数据区 `memcpy`。现在**内核侧几何一律用编译期
  `VDRM_AUDIO_RING_SIZE`**，`open()` 时把 `magic/size/rate/channels/format`
  重新写回，`GET_INFO` 也只报常量。
- **`.ack` 处理 appl_ptr 回退**（`snd_pcm_rewind`，PulseAudio 会用）：无符号差值
  回绕会被钳成整 buffer，把 32KB 已播数据再灌一遍。现在按有符号差判回退，
  回退时只同步游标、不拷数据。
- **`pointer` 的 hw_ptr 只许前进**：ALSA 把"新值比旧值小"当回绕并伪造整 buffer
  前进；rewind 后必须钳住。
- **`vdrm_pcm_close` 用会等待的 `hrtimer_cancel()`**（close 在可睡眠上下文，
  紧接着 ALSA core 就 `kfree(runtime)`）；trigger 路径仍用 `try_to_cancel`。
- **`g_users` 的写入放进 `g_lock`**：旧版 release 裸写，会盖掉并发的 open，
  之后驱动永远以为"没人收"（一路丢帧且不可恢复）。
- **`open()` 不再直接改 `head`**（那是 push 的单写者字段，两边锁不同）。改为置
  `g_resync`，由持流锁的 `ack`/`pointer` 补做"head 追平 tail"。
- **96KB 上限在单次 push 内也生效**（旧版只在循环开头判一次，一次最多能冲过
  上限一个 period）。
- **声明 `SNDRV_PCM_INFO_SYNC_APPLPTR`**：mmap 模式下 appl_ptr 由用户态直写、
  无 syscall 无屏障，`ack` 不会被调；声明之后 alsa-lib 走 `SYNC_PTR`，
  `ack` 会被调，也才有"样本写完再提交指针"的顺序保证。

验证：两档改动在 DDK `android14-6.1` 上 `-j8` 全量重编 **0 warning**（BTF 步骤仍
按本仓库惯例失败，需手工 `ld.lld` 重链，见 README/CHANGES 上文）。设备侧
`insmod`/`rmmod` 冒烟待 adb 恢复后补。

## 本版本第九项改动：设备实测暴露的三个问题

前两项是纯代码审查看不出来的，只有在本机（PKG110，6.1.157-android14-11）真正
`insmod` 才暴露：

### 1. 官方 release 的 6.1 .ko 在本机内核上根本加载不了

`vdrm-android14-6.1.ko`（weekly-2026-W43）里有两条未定义符号在本机内核上**没有
导出**：

```
vdrm: Unknown symbol snd_ctl_free_one (err -2)
vdrm: Unknown symbol snd_pcm_lib_default_mmap (err -2)
```

两者都存在（/proc/kallsyms 有），但不在模块可用的导出表里 —— GKI 6.1.166 的
`Module.symvers` 里有、这台设备的 6.1.157 内核没有。`insmod` 直接 ENOENT。
处理：

- `.mmap = snd_pcm_lib_default_mmap` 删掉。`substream->ops->mmap == NULL` 时
  ALSA core 自己会调 `snd_pcm_lib_default_mmap()`，行为一模一样，模块不再引用它；
- 错误路径的 `snd_ctl_free_one(ctl)` 删掉（只在 ENOMEM 时漏一个控件）。

> 结论：这个 release 资产在本机已经不能用了；以后 CI 出的包也要按这个标准
> （只引用设备内核确实导出的符号）验一次。

### 2. 音频环的 order-8 连续内存经常拿不到 → 整条音频静默失效

`alloc_pages_exact(528384)` 需要 256 页物理连续。本机实测直接 `-ENOMEM`，
`vdrm_audio_init` 只打一行 `audio disabled` 就继续 —— 声卡、`/dev/vdrm-audio`
全都没有，而模块看起来"加载成功"。

改成 `vmalloc_user()` + `remap_vmalloc_range()`：

- 不再要求物理连续；
- mmap 走 `vm_insert_page` 逐页映射（页有引用计数），不再是"把一段物理地址
  原样映射给用户态"；
- 共享页布局、ABI、App 侧的用法都不变。

### 3. 池缓冲每次 mmap 都泄漏一份 GEM 引用（卸载时 `drm_mm_takedown` WARN）

`drm_gem_mmap_obj()` 为每次映射 `drm_gem_object_get(obj)`，等的是 `vm_close`
归还；但 `vdrm_pool_mmap()` 把 `vma->vm_ops` 换成 `dma_buf_mmap()` 的
（vma 绑到 dmabuf 的 file 上），那个 close 不认识 GEM —— 这份引用永远没人还。

后果：池对象（gralloc AHB，1920x1080 约 8 MB/块）泄漏，`mmap offset` 节点留在
`drm_mm` 里，卸载时 `drm_mm_takedown()` WARN *"Memory manager not clean during
takedown"*。实测：合成器每 mmap 一次漏一块，WARN 每次卸载必现。

修：`dma_buf_mmap()` 成功后立刻 `drm_gem_object_put(obj)` —— 映射本身由 dmabuf
的 file 引用续命，GEM 那份引用本来就不该由 vma 持有。

### 冒烟测试（`test/vdrm_smoke.c`）

freestanding aarch64（裸 syscall，无 libc），`insmod` 后跑：GET_INFO / GET_POOL /
dma-heap 注册 / fd 原样转发 / `CREATE_DUMB`(池) / `MAP_DUMB` + `mmap` 读写 /
池空 `-ENOSPC` / 占用中 `UNREGISTER` = `-EBUSY` / postclose 后可清 / 输入批量
no-op / 音频环 `GET_INFO` + `mmap` + 环头校验。

本机结果：**24 pass / 0 fail**，`insmod`→冒烟→`rmmod` 全流程无 WARN/Oops，
`drm_mm_takedown` 计数不再增长。

## 本版本第十项改动：给虚拟卡挂一个 render 节点（"牌子"）

### 为什么

虚拟卡只有 card1，`drmGetDevices2()` 里它是一台"没有 render 节点"的设备。
合成器/libdrm 的配对逻辑（按 primary + render 成对）和一部分工具会因此把
它当异常；实测 `drmGetDevice2()` 读两个节点时 `available_nodes` 只有
`1<<PRIMARY`。

加一个 render 节点之后（`DRIVER_RENDER`），`drmGetDevices2()` 返回的是
**一台设备、两个节点**：

```
dev[0] nodes=0x5 primary=/dev/dri/card1 render=/dev/dri/renderD129
```

`drmDevicesEqual(card1, renderD129) == 1`，合成器不需要再手工配"显示 + 渲染
两个路径"。

### 它不做什么（重要）

这个节点**没有任何执行力**：虚拟卡后面没有 GPU。真正的渲染在用户态 —— 实测
mesa 在 card1 上初始化 EGL 时，不论 `MESA_LOADER_DRIVER_OVERRIDE` 是 `msm`、
`kgsl` 还是不设（默认 zink+Turnip），**最终都打开 `/dev/kgsl-3d0`**，也就是
真 GPU 的提交口。render 节点只用于 DRM core 的 render 类 ioctl。

### 实现

- `g_driver.driver_features |= DRIVER_RENDER`，由模块参数
  `render_node=1`（默认开）控制，可 A/B 回退成单节点；
- 不需要新的 ioctl 回调：DRM core 的 `CREATE_DUMB`/`MAP_DUMB` 本身就带
  `DRM_RENDER_ALLOW`（`drm_ioctl.c:681-682`），所以 render 节点**也能从同一个
  池里发缓冲**；`PRIME_FD_TO_HANDLE`/`GEM_CLOSE` 同样可用；
- 池语义不变：render 节点发出去的槽同样 `allocation_reserved`，关闭那个
  drm_file 时由 `postclose` 归还。

### 验证（本机 6.1.157）

- `render_node=0` → 只有 card1；默认 → card1 + renderD129（226:129）；
- `drmDevicesEqual = 1`，`drmGetDevices2` 显示成对（见上）；
- 冒烟扩到 **29 项全过**，新增：
  ```
  PASS render: CREATE_DUMB ok
  PASS render: slot reserved -> UNREGISTER EBUSY
  PASS render: PRIME import ok
  PASS render: GEM_CLOSE ok
  PASS render: postclose released slot
  ```
- `insmod`/`rmmod` 两轮，两个节点一起出现/消失，`drm_mm_takedown` 无新 WARN。

## 本版本第十一项改动：音频 hw_ptr 的两处记账错误（真的会卡死播放）

前一轮只做了"别把内核挂住"的防御，没验过"连续播"。这次拿 libasound 直接写
`hw:0,0` 复现出来两个独立的 bug，一起修：

### 症状

- 第一次播一段（0.5 s）能过；
- 再开一条流：写满 8192 帧（正好一个 ALSA buffer）之后 `snd_pcm_writei`
  等 10 秒拿 `-EIO`，`drain` 也 `-EIO`；
- 而 `ring_used_bytes` 是 **0** —— 环是空的，所以不是"App 没收"。

### 根因 1：App 重连时的 resync 把这一轮的帧吞了

`vdrm_pcm_ack_push()` 里 App 打开时置的 `g_resync` 分支干了：

```c
smp_store_release(&g_ring->head, tail);
g_last_appl = appl_ptr;          /* ← 就是这行 */
```

`g_last_appl` 被提前对齐到本次的 `appl_ptr`，紧接着的

```c
delta = appl_ptr - g_last_appl;  /* == 0 */
if (!delta) return false;        /* 一帧都没推 */
```

于是那 512/1024 帧明明写进了 `dma_area`，却既没进环、也没记账。环空 +
hw_ptr 不动 + 容器写完一个 buffer 后永远等 → 10 秒 `-EIO`。

修法：resync 只丢"环里 App 没取走的旧数据"，**这一轮待推的帧照推**：

```c
u32 pending = appl_ptr - g_last_appl;
smp_store_release(&g_ring->head, tail);
g_last_tail = tail;
g_consumed_bytes = (u64)(appl_ptr - pending) * fb;   /* 丢弃的算已消费 */
g_last_hw = appl_ptr - pending;
/* g_last_appl 不动，后面 delta = pending 正常推 */
```

### 根因 2：hw_ptr 跟着"App 取走多少"走，等于把播放瞬间播完

修完根因 1 后数据能进环了，但**写完第一小段就 XRUN**（`state=4`，
之后所有 `writei` 都是 `-EPIPE`）。原因是我们的 `pointer()` 直接返回
App 的消费进度：App 拿到数据是立刻交给 AAudio 的（微秒级），
512 帧（10 ms）在几十微秒内就"播完"了，于是 `hw_ptr == appl_ptr`、
`avail == buffer_size >= stop_threshold`，ALSA 按"缓冲空了"判 underrun。

真声卡上 hw_ptr 有两个约束同时成立：不超过已写入的量（`appl_ptr`），
也不超过真实时间的进度。补上后者：

```c
hw = consumed;                                    /* App 的背压 */
if (rt->state == RUNNING && rt->rate) {
        u64 ns = ktime_get_ns() - g_hw_t0;        /* START 时记的原点 */
        u64 timed = g_hw_base + ns 换算成帧;
        if (timed < hw) hw = timed;               /* 时间不许超前 */
}
```

App 跟不上时 `consumed < timed`，hw_ptr 退化成消费者进度 —— 容器写满照样被
挡回去，背压还在。

### 验证（本机 6.1.157，容器里用 libasound 直写 hw:0,0）

```
$ python3 aprobe2.py            # 48000 帧（1 秒）分 47 次写
 10240/48000 frames t=0.04s ring=25344 state=3
 20480/48000 frames t=0.31s ring=27904 state=3
 30720/48000 frames t=0.50s ring=30464 state=3
 40960/48000 frames t=0.74s ring=25344 state=3
done written=48000 errs=0 wall=0.86s state=3
drain 0
```

连跑三条，`errs=0`、`dropped_bytes=0`、`discarded_bytes=0`，
`st_push_bytes=576000`（= 3 × 48000 × 4，一字节不差），
`ring_used_bytes` 回到 0。wall 0.86 s vs 数据 1.0 s —— 时间钳位生效，
不再"瞬间播完"。

### 顺手加的诊断参数

出问题时先分清"数据进没进环"和"hw_ptr 有没有被推"，所以把回调计数暴露出来
（都是只读、只增）：

| 参数 | 含义 |
|---|---|
| `st_ack_calls` | ALSA `.ack` 被调次数 |
| `st_ptr_calls` | `.pointer` 被调次数 |
| `st_kick_calls` | App 的 `VDRM_AUDIO_KICK` 次数 |
| `st_start_calls` | trigger START/RESUME 次数 |
| `st_resync` | App 重连时的环重同步次数 |
| `st_early_delta` | ack/pointer 进来时 `appl_ptr` 没变的次数（正常，别慌） |
| `st_early_nodma` | 没拿到 `dma_area` 就进来的次数（异常） |
| `st_early_nousers` | 没有消费者时直接丢的字节数（≈ App 没开） |
| `st_push_bytes` | 真正推进共享环的字节数 |
| `st_users` | `/dev/vdrm-audio` 当前打开数 |

## 本版本第十二项改动：plane 报 IN_FORMATS + LINEAR（以及一个把手机搞重启的坑）

### 做了什么

`drm_simple_display_pipe_init()` 本来就有一个 `format_modifiers` 参数，之前传的是
`NULL`。改成传 `{ DRM_FORMAT_MOD_LINEAR, DRM_FORMAT_MOD_INVALID }` 之后：

- 内核给 plane 建了 `IN_FORMATS` blob，`DRM_CAP_ADDFB2_MODIFIERS` 也为真；
- 合成器不用再走"带 modifier 失败 → 退回 invalid modifier"那条退路；
- 内核自带的 `drm_simple_kms_format_mod_supported()` 本来就只认 `LINEAR`，
  dumb buffer 也是线性，报这个和实现一致，不是谎报。

设备上验（`/tmp/informats` 直接读 blob）：

```
/dev/dri/card1: ADDFB2_MODIFIERS=1
plane 32: IN_FORMATS 64 bytes, formats=4 modifiers=1
  format 0x34325258 / 0x34325241 / 0x34324258 / 0x34324241   (XR24/AR24/XB24/AB24)
  modifier[0]=0x0 (LINEAR) formats_mask=0xf offset=0
```

niri 照常出图（`Virtual-3` 1920x1080@120，`frames_pushed` 正常增长，
`frames_unmatched=0`）。日志里剩下的 `will try with invalid modifier` 全部是
**card0**（真机 GPU，容器里不是 master，addfb 返回 ENODEV），跟 card1 无关。

### 坑：modifier 数组必须有 `DRM_FORMAT_MOD_INVALID` 哨兵

`drm_universal_plane_init()` 数 modifier 个数的写法是

```c
	while (*temp_modifiers++ != DRM_FORMAT_MOD_INVALID)
		format_modifier_count++;
```

不像 formats 那样另有 count 参数 —— 它**靠哨兵停**。第一版只写了
`{ LINEAR }`，内核一路往后读，读到模块 `.rodata` 之后的未映射页：

```
Unable to handle kernel paging request at virtual address ffffffe865ffd000
pc : __drm_universal_plane_init+0x520/0x534
Call trace: drm_simple_display_pipe_init -> vdrm_do_pipe -> init_module
Kernel panic - not syncing: Oops: Fatal exception
```

手机直接重启（`insmod` 里 panic）。更阴的是它**不是必现**：开机那次加载时
数组后面那页恰好是映射的，读到的垃圾被当成 modifier 用了；重载时那页没映射就炸。
所以"能开机"完全不能作为"没 bug"的证据 —— 这类越界必须从契约上修死。

## 本版本第十三项改动：connector 报一份合成 EDID

虚拟 connector 之前没有 EDID：niri 日志一直喊
`error getting EDID info for Virtual-3: no EDID property`，而且拿不到物理尺寸。

现在在 `vdrm_do_pipe()` 里自己挂 EDID 属性 + 塞一份 128 字节 base block：

- 头/厂商 `VDR`/产品码/序列号/EDID 1.4/数字输入 8bpc/物理尺寸/gamma；
- 一个 detailed timing 描述符 —— 数字**来自 `vdrm_timing_fill()`**，和
  `get_modes()` 造 mode 用的是同一份参数，所以 EDID 里的 preferred timing 和
  driver 报的 mode 严格一致（不会让合成器选到别的分辨率，dumb_create 才不会
  和池对不上）；
- display range limits（48–refresh Hz，30–行频，CVT）、产品名 `VDRM`、
  序列号 `vdrm-virtual`，最后补校验和。

有个坑要记：`drm_connector_init()` 只给**非** `VIRTUAL`/`WRITEBACK` 的
connector 挂 EDID 属性（core 里的条件判断），虚拟 connector 必须自己调
`drm_connector_attach_edid_property()`，否则 `drm_connector_update_edid_property()`
设了也没人看得见。

设备实测：

```
$ ./edid /dev/dri/card1
connector 31 type=15 mm=160x90 modes=1
  mode 1920x1080@120 clock=297000
  EDID blob 128 bytes, head: 00 ff ff ff ff ff ff 00
  name: VDRM
  timing: 1920x1080 clock=29700
```

niri 这边：

```
Output "PNP(VDR) VDRM vdrm-virtual" (Virtual-3)
  Current mode: 1920x1080 @ 120.000 Hz (preferred)
  Physical size: 160x90 mm
```

`no EDID property` 那两条还在，但都是 **Virtual-1/Virtual-2**（card0 真机 GPU 的
虚拟 connector，容器里本来就不该碰），我们的 Virtual-3 已经干净了。
`frames_pushed` 正常增长、`frames_unmatched=0`。

## 本版本第十四项：SYNCOBJ —— 查清了，但**故意不报**

niri 日志里有一条 `DRIVER_SYNCOBJ absent`（退化路径）。把内核这条链路读完之后
的结论是：**只把 `DRIVER_SYNCOBJ` 标志位加上去是有害的，不能加。**

理由（对着 6.1 源码）：

- syncobj 本身的 ioctl（create/destroy/wait/timeline）只由
  `drm_core_check_feature(dev, DRIVER_SYNCOBJ)` 把关，内核通用实现，加标志就能用；
- 但合成器真正要的是**原子提交的 out-fence**：`DRM_IOCTL_MODE_ATOMIC` 带
  `OUT_FENCE_PTR` 属性时，`drm_atomic_uapi.c` 会 `drm_crtc_create_fence(crtc)`
  造一个 `dma_fence`，`setup_out_fence()` 把它的 fd 交给用户态；
- 全树搜下来，**没有任何代码在翻页完成时 signal 这个 crtc fence**
  （`drm_crtc.c` 只负责 init/alloc，`drm_crtc_send_vblank_event()` 里也没有）。
  也就是说：谁报了这个能力，谁就得自己在 commit 尾/中断里 signal 它 ——
  而这一版 uapi 造出来的 fence 是驱动拿不到的（存在 uapi 的 fence_state 里）。
- 简单显示管线（`drm_simple_display_pipe` + vblank 事件）没有这条路径。
  如果我们硬加上标志，合成器会拿到一个**永远不会完成的 out-fence**，
  等起来就是卡死 —— 比现在这条 "degraded path" 警告糟得多。

所以：保持不报。翻页完成的语义我们已经用 `drm_crtc_send_vblank_event()` /
`drm_crtc_arm_vblank_event()` 走通了（`flip_done_immediate=0` 时按 vblank 网格
完成），这是 simple-pipe 驱动文档里的标准做法。真要做 explicit fencing，
得自己实现 `atomic_commit` 并在完成时 signal 一个自己持有的 fence，
属于另一件大改，收益只是消掉一条警告 —— 不值。

## 本版本第十五项：DPI 变成可调的旋钮（`edid_dpi`）

EDID 报的物理尺寸不只是"信息"，它直接决定合成器算出来的 DPI，进而决定合成器
自己的 scale。niri 实测是线性的：

| `edid_dpi` | EDID 物理尺寸 | niri scale | 1920 宽时的逻辑分辨率 |
|---|---|---|---|
| 128 | 381x214 mm | 1.0 | 1920x1080 |
| 192 | 250x140 mm | 1.5 | 1280x720 |
| 256 | 190x100 mm | 2.0 | 960x540 |
| 305（默认，160mm）| 160x90 mm | 2.25 | 853x480 |
| 384 | 127x71 mm | 3.0 | 640x360 |

关系就是 `scale ≈ dpi / 128`（niri 取到 0.25）。所以"UI 想多大"可以直接用
`dpi=` 表达，不用去改合成器配置：

```
# mode.conf
mode=1920x1080
dpi=256          # → 逻辑 960x540
```

`module/common.sh` 会把它翻译成 `insmod … edid_dpi=256`。

一点提醒：这个 DPI 是**给合成器的缩放建议**，不是"手机屏幕的真实尺寸"。
真正的观感还取决于 App 把虚拟帧 buffer 铺到屏幕的方式 —— 1920 模式是缩到
1080 宽（0.5625 双线性，字会略软），2376x1080 模式是 1:1（锐，但会铺满、
摄像头挖空压在桌面上）。

## 本版本第十六项：vblank 网格改成按需启动（空闲不再 120 Hz 空转）

### 之前

第一次 `enable_vblank` 之后 hrtimer 就一直跑下去，只是用 `g_vblank_on` 决定
要不要真的 `drm_crtc_handle_vblank()`。也就是说：即使合成器完全空闲、没人等
vblank、也没 flip 在飞，内核还是每秒醒 120 次（每次 spinlock + wake_up）。

当初不敢停，是因为注释里写的那条担心：停了再起会从"现在"重新算一个整周期，
vblank 相位被打断 —— 那正是"每帧白等一个刷新周期"的来源。

### 现在

- `enable_vblank`（core 的 vblank 引用 0→1）→ 起网格；
- `disable_vblank`（1→0）→ 停网格；
- 相位不丢：一直维护**下一个 vblank 的绝对时刻** `g_vblank_next`，停机期间
  时间照走，下次启动时把它往后推到第一个未来边界，接回原节拍；
- 定时器回调里用 `hrtimer_set_expires()` + `HRTIMER_RESTART` 按绝对时间推进；
- 停网格只从 `disable_vblank`（持 event_lock + vbl_lock 的自旋锁上下文）里
  调用，所以只能用不等待的 `hrtimer_try_to_cancel()`；回调如果在别的 CPU 上
  正跑着，它会看到 `g_vblank_timer_running == 0` 而不再续期 —— 不会死锁。

flip-done 的语义没变：`pipe_update()` 里 `drm_crtc_vblank_get()` 拿到引用、
把事件 arm 到下一个 vblank，事件发完 core 会 `vblank_put()`，引用归零后网格
自动停 —— 一次 flip 恰好铺满一个"起-停"周期。

### 验收（设备实测）

```
insmod 后立刻:      running=0  ticks=0
niri 起来画首屏:    running=0  ticks=18  frames=18
再开一个窗口:       running=0  ticks=34  frames=34   unmatched=0
空闲 4 秒后:        running=0  ticks=34              ← 不涨了
```

`vblank_ticks` 和 `frames_pushed` 一一对应，空闲时彻底停住。niri 日志里没有
任何 `flip_done timed out` / commit 超时，剩下的告警都是原来那几条
（card0 的 framebuffer/leasing、真机 touchpanel 的 libinput bug）。

## 本版本第十七项：同一分辨率下多档刷新率（运行时能切）

### 之前

`get_modes()` 只造一个 mode（配置的那个刷新率）。想换刷新率只能改 `refresh=`
参数、重载模块、重建池、重启合成器。

### 现在

同一分辨率下对外报三档：配置的那档（preferred）+ 120 + 90 + 60（去重）。
**分辨率不变**，所以池、App、dumb_create 全都不受影响 —— 合成器可以运行时切：

```
$ niri msg output Virtual-3 mode "1920x1080@60.000"
$ niri msg outputs | grep "Current mode"
  Current mode: 1920x1080 @ 60.000 Hz
$ dmesg | grep pipe
  vdrm: pipe disable
  vdrm: pipe enable 1920x1080 period=16666666 ns   ← vblank 网格自动跟着换
```

`pipe_enable()` 本来就从 `crtc_state->mode` 反推 `period_ns`，所以切刷新率时
vblank 节拍、flip-done 时序、按需起停都是自动的，不需要额外代码。

实测：120→60→120 来回切，`frames_pushed` 正常增长、`frames_unmatched=0`，
`vblank_ticks` 与新周期一致。

### 还没做的：运行时切**分辨率**

分辨率一变，池里 slot 的尺寸就不对了（`dumb_create` 只能从池里发同样尺寸的
slot），必须让 App 重建池：`UNREGISTER id=0` 清池 → 用新尺寸重跑 `vdrm_mkpool`
→ App 重新 forge。App 那半（监视 GET_INFO 的宽高变化并重建池）需要重编
`libvdrm_consumer.so`，容器里目前没有 NDK，所以先留着。

## 本版本第十八项：运行时切分辨率（不用重载模块 / 不用重启）

### 内核侧

`mode` 参数变成可写的（`echo 2376x1080 > /sys/module/vdrm/parameters/mode`）：

1. 更新 `g_mode_w/h/pitch`；
2. 用新几何重建 EDID（preferred timing、物理尺寸一起变）；
3. `drm_kms_helper_hotplug_event()` 通知用户态重新探测模式列表；
4. `mode_gen++`，并唤醒正在 `dumb_create` 里等新尺寸槽的合成器。

`dumb_create` 也改成**等一下**：合成器往往比池重建更早来要新尺寸的 buffer，
以前是直接 `-ENOSPC`。现在如果请求的尺寸等于当前 mode，就最多等 3 秒
（每次唤醒/超时重扫一遍池），不是当前 mode 的尺寸才立刻失败。

### 池重建：交给模块的 root watcher，而不是 App

App 是 `untrusted_app`，让它 `fork`+`su` 去跑 `mkpool` 不一定通（实测没跑起来，
App 只等到超时）。改成：

- 内核暴露只读参数 `mode_slots`（池里尺寸等于当前 mode 的槽数）；
- 模块的 `input_watch.sh`（root，常驻）每秒看一眼：少于 4 个就重跑一次
  `vdrm_mkpool`（每 2 秒最多一次）。这样它看的是**真实状态**，不会在合成器
  还攥着旧槽时白跑；
- `UNREGISTER id=0`（清池）现在会**等**占用中的槽放掉（最多 3 秒）再清 ——
  否则切分辨率时清不干净，mkpool 只能拿到 4/6。

App 那边只负责：发现 `GET_INFO` 的宽高变了 → 丢掉旧 AHB → 等池出现后重新
forge（最多 10 秒重试）。它不再尝试自己起 mkpool。

### 合成器那边要补一条命令

热插拔后 niri 会把输出**关掉**（它的"当前模式"从列表里消失了），不会自己挑
新模式。所以切完要补一句：

```
$ echo 2376x1080 > /sys/module/vdrm/parameters/mode
$ niri msg output Virtual-3 on        # 或 mode "2376x1080@120.000"
```

### 实测（本机，niri 一直在跑，没有重启）

```
1920x1080 @120  ->  echo 2376x1080 > mode
   watcher: 池缺 2376x1080 的槽（现 0 个）-> 重建
   mkpool:  6/6 registered
   app:     pool rebuilt for 2376x1080 (6 bufs)
   niri:    Current mode: 2376x1080 @120   frames 增长 / unmatched=0
2376 -> 1920 同样一轮，slots=6、frames_unmatched=0。
```

## 本版本第十九项：音频 capture（麦克风进容器）—— 内核半边

之前只有播放。现在多了一条 capture PCM（card0 device 1）和一个新的字符设备
`/dev/vdrm-audio-in`，方向反过来：**App 把 Android 侧录到的帧写进共享环，
内核的 capture PCM 从环里读给容器**。

### 实现要点

- capture 不假装 DMA：`runtime->dma_area` 直接指到共享环的数据区、
  ALSA buffer 大小被 `snd_pcm_hw_constraint_minmax` 钉成和环一样（64 KB =
  16384 帧），ALSA core 从 dma_area 把数据 copy 给用户，省掉一次内核拷贝；
- 采集环的 head 是 App 的写位置、tail 是内核的读进度（= ALSA 的 appl_ptr）；
- App 写完一段 ioctl(KICK) → 内核 `snd_pcm_period_elapsed()` 把阻塞的读者叫醒；
- 没有 App 喂数据时返回**静音**，不让容器的录音永远 block。

### 三个实测踩出来的坑

1. **capture PCM 必须在 `snd_card_register()` 之前建**。放在之后建不会报错、
   卡也照样起来，但 `/proc/asound/devices` 里根本没有 `0-1 digital audio
   capture`，`/dev/snd/pcmC0D1c` 也不会出现；
2. **hw_ptr 不能直接拿环内 head 换算**。head 是环内偏移，会回绕、也会在 App
   重开时被重置；直接返回会让 hw_ptr 往回跳，ALSA 把它当成"回绕 + 巨大
   delta"，立刻判 overrun（`-EPIPE`，实测读 15360 帧后就炸）。改成和播放侧
   同一套：只把"新增的那段"累加进一个**绝对帧数** `g_cap_written`；
3. **静音模拟不能每次 `pointer()` 都推进一个 period**。`avail_update` 多调
   几次就会比 appl 超前一个 buffer，同样触发 overrun。改成"只有 appl 追上来
   才往前推一个 period"。

### 验证（容器里用 libasound 直接读 hw:0,1）

模拟 App 的喂数脚本（往 `/dev/vdrm-audio-in` 按实时节奏写 440Hz 正弦）：

```
静音路径（不喂）: read 24576 frames, peak=0,    errs=0
喂音路径        : read 24576 frames, peak=9000, errs=0     ← 正弦幅度 9000
                  fed 144000 frames（3 秒），cap_overrun=0
```

### 还没做的：App 侧

App 现在还不会把麦克风录进环，所以容器录音拿到的是静音。App 侧要做的是：
打开 `/dev/vdrm-audio-in`、mmap、用 AAudio 开一个 INPUT 流、把读到的帧写进环
（按 tail 算空间）、每段 ioctl(KICK)。这部分等下一轮补。

### 补记：App 侧接上之后的第四个坑 —— tail 必须写成环内偏移

App 打开 `/dev/vdrm-audio-in`、用 AAudio INPUT 录麦克风写进环之后，容器里的
`read()` 读满一个 buffer 就卡住，25 秒后 ALSA 报
`-EIO "capture error (DMA or IRQ trouble?)"`。

根因在 `.pointer` 里把读进度写回环的那一行：

```c
	smp_store_release(&g_cap_ring->tail, appl * fb);   /* appl 是单调帧数 */
```

`appl` 会一直增长，`appl * fb` 迟早超过环大小（64 KB）。App 那边算空间用的是
`used = head - tail`（都是环内偏移），tail 一旦大于 size，无符号差就下溢成
"环满"，App 从此不再写；读者等在空环上，ALSA 等到超时就报 -EIO。

改成 `(appl * fb) % VDRM_AUDIO_IN_RING_SIZE` 即可。

### 最终验证（真机麦克风）

App 侧：AAudio INPUT 流（48 kHz / 2ch / VOICE_RECOGNITION）→ 每段写进环并按
tail 算空间 → `ioctl(KICK)` 叫醒读者。容器里用 libasound 读 `hw:0,1`：

```
opened hw:0,1
set_params 0 prepare 0 start 0
read 98304 frames, peak=4688, errs=0      ← 2 秒真实环境音
内核侧: written=98304 overrun=0 head=0
```

`cap_written` 和容器读到的帧数完全一致，overrun=0。

## 本版本第二十项：修"切分辨率时画面横向拉伸"

用户报告：容器里分辨率一变，会有一小段时间整屏被横向拉长/压扁。

根因不在内核，在 App 的图层几何：`sc_set_geometry()` 用的是**模块的 mode** 宽高。
运行时 `echo 2376x1080 > .../parameters/mode` 之后，模块 mode 立刻变了、2376 的新
buffer 还没来，而 App 这时又按新 mode 重算了一次 letterbox —— 正在显示的那块
1920 宽 buffer 就被按 2376 的宽高比拉伸，直到第一帧新尺寸到达。

修法：几何一律按**正在显示的那块 buffer** 的真实宽高算。

- `sc_latch()` 里 `AHardwareBuffer_describe()` 拿尺寸，存成 `g_last_buf_w/h`；
- `sc_geometry()`（窗口/模式变化时单独下发几何的那条路）用 `g_last_buf_*`，
  只有还没贴过任何帧时才退回传入的 mode；
- 于是切分辨率期间旧画面按自己的比例 letterbox，不会变形；新 buffer 一到，
  几何自然跟着换。

顺带记录一个使用注意：热插拔之后 niri 会把输出置为 Disabled，且实测
`niri msg output Virtual-3 on` / `mode …` **不一定救得回来**（它内部还记着
"connector mode list missing current mode"）。所以现在运行时切分辨率的完整流程是：

```
echo 2376x1080 > /sys/module/vdrm/parameters/mode   # 池由 watcher 自动重建
# niri 需要重启一次才能稳定接上新模式
```

## 本版本第二十一项：切分辨率不再需要重启合成器（mode 列表改成固定的）

之前 `mode` 一变，`get_modes()` 报的**整个模式列表**就跟着变，还发 hotplug ——
合成器一看"当前 mode 不在列表里"，就把输出关了。这是设计问题，不是正常行为：
真机上切分辨率就是合成器在**固定的一组** mode 里挑一个、做一次 atomic
modeset，不需要热插拔。

### 现在

- `get_modes()` 报**固定**的两档分辨率 × 三档刷新率：
  1920x1080 / 2376x1080 × 120/90/60（`mode` 参数只决定哪一个是 PREFERRED）；
- 合成器切模式 = 普通 modeset，**没有 hotplug**；
- 内核新增"生效尺寸" `g_active_w/h`：
  - `pipe_enable()`（合成器真正编程的模式）和 `dumb_create()`（提前来要 buffer）
    都会更新它；
  - `GET_INFO` 报它 → App 按它重建池、算触摸映射；
  - `mode_slots` 也按它数 → watcher 看到当前尺寸的槽不够就重跑 mkpool；
- `dumb_create` 对新尺寸的等待放宽到 6 秒，watcher 轮询改成 0.5 秒 ——
  给池重建留够时间。

只报两档是**池容量**决定的：池最多 16 槽、每档 6 个，三档就装不下（实测
1280x720 那次切不过去）。

### 实测（niri 全程不重启）

```
$ niri msg output Virtual-3 mode "2376x1080@120.000"   # 直接切，无 hotplug
$ niri msg outputs | grep Current
  Current mode: 2376x1080 @ 120.000 Hz
$ niri msg output Virtual-3 mode "1920x1080@120.000"
  Current mode: 1920x1080 @ 120.000 Hz
```

1920↔2376 连续切 4 次 + 3 次全部生效，`mode_slots` 每次都回到 6，
`frames_unmatched=0`。切换瞬间会有 1–2 秒的池重建（mkpool 会清整池），
以后让 mkpool 保留多档尺寸就能做到即时切换。

## 本版本第二十二项：池扩到 30 槽、四档分辨率、"全面屏"不写死

### 槽位与分辨率

- `VDRM_POOL_MAX` 16 → **32**（30 个目标 + 余量）；
- 对外广告**四档**分辨率：`1080x1920`、`1920x1080`，外加 App 上报的窗口尺寸
  的**横竖两档**（这台是 `2780x1264` / `1264x2780`），每档三档刷新率 =
  12 个 mode；
- 合成器在这四档里随便切 = 普通 atomic modeset，**不 hotplug、不重启**。

### "全面屏"怎么不写死

App 在 `nativeSurfaceChanged` 里把自己的**窗口真实尺寸**（`ANativeWindow_getWidth/
getHeight`）通过新 ioctl `VDRM_IOCTL_SET_SCREEN` 上报给模块；模块据此重建
分辨率列表，并通过只读参数 `modes` 暴露出去：

```
$ cat /sys/module/vdrm/parameters/modes
1080x1920,1920x1080,2780x1264,1264x2780
```

这样机型分辨率、`wm size` override、以后分屏，都不需要改代码。

### mkpool 改成"保留 + 补齐"

以前每次跑都先清整池，所以切到一个新尺寸要等一整轮全清全建（1–2 秒黑）。
现在：

- 默认**不清池**，先查 `/sys/module/vdrm/parameters/mode_slots` 看当前尺寸已有
  几个，只补缺的（目标 6 个）；
- 其它尺寸的槽原样留着 → 来回切几乎零成本；
- `vdrm_mkpool --reset` 才清池（手动兜底用）。

### 实测（niri 全程不重启）

```
1080x1920 -> 生效  slots=6  frames_unmatched=0
2780x1264 -> 生效  slots=6  frames_unmatched=0
1264x2780 -> 生效  slots=6  frames_unmatched=0
1920x1080 -> 生效  slots=6  frames_unmatched=0
再切回已建过池的档: 0.1 秒生效（首次切到新尺寸约 1–2 秒，要补 6 个槽）
```

内存账：四档 × 6 槽 ≈ 268 MB（这台 15.6 GB，约 1.7%），可接受。

## 本版本第二十三项：常驻 watcher 从"每秒轮询"改成"事件驱动"

之前模块里常驻一个 `input_watch.sh`，每 0.5–1 秒扫一次 `/sys/class/input` 和
`mode_slots`：功能没问题，但空闲时也在空转。

现在内核加了一条**控制事件通道**：

- 新 ioctl `VDRM_IOCTL_SET_CTLFD`：用户态把自己创建的 eventfd 注册进来；
- 内核在这些时刻 `eventfd_signal()`：
  - 三个 evdev 设备注册完（该去把 `/dev/input` 的节点换成 Android 打不开的）；
  - 生效分辨率变了（`pipe_enable` / `dumb_create` 更新 `g_active_*` 时）；
  - watcher 刚注册上（立刻跑首轮）；
- 模块里配了一个 8 KB 的小工具 `vdrm_ctlwait`：注册完就 `poll()` 阻塞，
  来事件打一行 `event`，5 秒没事件打一行 `tick`（心跳，为了模块重载后能重新
  注册）；它**常驻不退出** —— 退出再重开会有窗口丢事件（内核那份引用还在，
  signal 就丢了）。

`module/vdrm_watch.sh` 变成：

```sh
"$MODDIR/vdrm_ctlwait" 5 | while read -r _; do
    vdrm_hide_input            # 输入节点
    [ mode_slots < 4 ] && vdrm_mkpool   # 池补齐
done
```

平时完全阻塞在 `poll()` 上（不耗 CPU），只有事件或 5 秒心跳才动一次；
没有 `vdrm_ctlwait` 时退回 2 秒轮询，少个文件也不会丢功能。

实测：四档分辨率来回切（含首次切到没建过池的档）全部生效，
`frames_unmatched=0`；已建过池的档再切 **0.10 秒**生效。

## 本版本第二十四项：修"屏幕全黑"—— 把 ioctl 结构大小冻成 ABI 常量

症状：第二十二项把池上限从 16 改成 32 之后，App 里一片黑（连虚拟键盘的底色都没有），
`logcat` 里是：

```text
VDRM: forge_pool: KPM pool empty (n=-1)   ×N
```

根因不在池，在 **ioctl 号**。`GET_POOL` 用的是 `_IOWR`，编号里编码了
`sizeof(struct vdrm_pool_list)`：

```c
#define VDRM_IOCTL_GET_POOL VDRM_IOC(3, 'V', 0x07, sizeof(struct vdrm_pool_list))
```

`struct vdrm_pool_list` 里内嵌 `items[VDRM_POOL_MAX]`，池上限 16 → 32 让结构从
~1 KB 变成 ~2 KB，ioctl 号随之改变。而 App 是**按自己编进去的旧号**去调用的：

- 旧号 ≠ 新号 → 内核走 `default:` 返回 `-ENOTTY`（`n=-1`）；
- App 于是「池是空的」，一块 buffer 也 forge 不出来，`dumb_create` 永远等不到槽；
- App 的 Surface 没东西可交 → 屏幕上就只剩系统底色，看起来是「全黑」。

这类错误不会崩、不会报错，只是**静默失效**，所以特别值得单独记一笔。

修法是把「ABI 看到的大小」和「内部能用的槽数」拆成两个常量：

```c
#define VDRM_POOL_MAX      32   /* 内部池上限，可以继续涨 */
#define VDRM_POOL_ABI_MAX  32   /* 【冻结】ioctl 号编码它，永远别改小/改大 */
#if VDRM_POOL_MAX > VDRM_POOL_ABI_MAX
#error "VDRM_POOL_MAX 不能超过 ABI 上限 VDRM_POOL_ABI_MAX"
#endif
struct vdrm_pool_list {
        u32 count;
        u32 reserved;
        struct vdrm_pool_info items[VDRM_POOL_ABI_MAX];   /* ← 用 ABI 常量 */
};
```

App 侧同名常量加注释（`VDRM_POOL_MAX 32`，对齐 `VDRM_POOL_ABI_MAX`），
两边都不许「顺手跟着内部池大小改」。

教训：**任何出现在 ioctl 号里的结构，参与编号的字段都不能随业务参数浮动**。
以后要扩容，只动内部上限；真需要更大表就新开一个 ioctl 号，别改老的。

实测（重载模块 + 重装 `.so` + 起 niri）：
`forge_pool: 6 forged from KPM pool (of 18)`、`frames=355 unmatched=0`，
桌面正常出画。

## 本版本第二十五项：修"合成器挑错模式导致黑屏"—— PREFERRED 与 DRIVER 位写反了

修完第二十四项（ABI 对齐）之后 App 已经能 forge 出 6 块 buffer，但屏幕**还是黑的**，
而且这次的现场完全不同 —— niri 的日志是：

```text
DEBUG niri::backend::tty: connecting connector: Virtual-3
DEBUG niri::backend::tty: picking mode: Mode { name: "1264x2780", ..., PREFERRED }
WARN  niri::backend::tty: error connecting connector: error creating DRM compositor
      0: The underlying allocator encountered an error: Invalid argument
```

两个信号：

1. niri 挑的是 `1264x2780`（全屏竖档），不是我们配置的 `1920x1080`；
2. `niri msg outputs` 里 **12 个模式全部标着 `(preferred)`** ——
   `1264x2780@120 ... 1920x1080@60` 每一档都有。

根因是 `get_modes` 里两个 `drm_mode.type` 位**写反了**：

```c
/* drm_mode.h：DRM_MODE_TYPE_PREFERRED = (1 << 3)，DRM_MODE_TYPE_DRIVER = (1 << 6) */
m->type        = 1 << 3;  /* 注释写的 DRIVER，实际是 PREFERRED ← 每档都成了 preferred */
if (配置的那档)
        m->type |= 1 << 6;  /* 注释写的 PREFERRED，实际是 DRIVER */
```

于是每个模式都带 PREFERRED。合成器按"第一个 preferred"挑（内核 `drm_mode_sort`
会把 preferred 打头、再按像素时钟降序），正好落在像素时钟最大的全屏档
`1264x2780@120`；而池里只有配置那档的槽，`dumb_create` 等 6 秒拿不到 → 分配失败
→ 连接器一直是 `Disabled` → App 没帧可显示 → 黑屏。

（顺带解释了一个之前的疑点：`/sys/class/drm/card1-Virtual-3/modes` 的顺序是
`1264x2780, 2780x1264, ...` 而不是我们 `g_modes` 的插入顺序 —— 那就是内核排序，
不是列表错了。）

修法：改成具名常量 + 正确的位，并把这条坑写进注释：

```c
#define VDRM_MODE_TYPE_PREFERRED (1 << 3)
#define VDRM_MODE_TYPE_DRIVER    (1 << 6)
#define VDRM_MODE_FLAG_PHSYNC    (1 << 0)
#define VDRM_MODE_FLAG_PVSYNC    (1 << 2)

m->flags = VDRM_MODE_FLAG_PHSYNC | VDRM_MODE_FLAG_PVSYNC;
m->type  = VDRM_MODE_TYPE_DRIVER;
if (w == g_mode_w && h == g_mode_h && r == 0)
        m->type |= VDRM_MODE_TYPE_PREFERRED;   /* 只有配置那档的首个刷新率 */
```

实测（重编 + 重载 + 起 niri）：

```text
niri msg outputs → Virtual-3 Current mode: 1920x1080 @ 120.000 Hz (preferred)   ← 只剩一个 preferred
niri msg output Virtual-3 mode 2780x1264@120.000
                → Current mode: 2780x1264 @ 120.000 Hz，画面铺满、文字 1:1 不拉伸
frames_pushed 持续增长，frames_unmatched=0
```

教训和第 24 项同源：**这类"位/编号写反"不会编译报错、不会崩，只会静默选错**。
凡是 uapi 里用位标志的东西，宁可多定义几个宏，也不要写裸字面量加注释。

## 本版本第二十六项：GET_POOL 对"结构大小"宽容（把这类黑屏从根上堵掉）

第二十四项只是把大小**对齐**了，同样的坑还留着：只要以后内部池上限再涨、
或者用户装的是旧版 App，两边结构大小又会不一致，ioctl 号又对不上，
又是「静默全黑」。所以干脆让驱动不认这个 size：

```c
/* 只比较 dir/type/nr，忽略 size 那 14 位 */
#define VDRM_IOC_BASE(cmd) ((cmd) & ~(0x3fffu << 16))
#define VDRM_IOC_SIZE(cmd) (((cmd) >> 16) & 0x3fffu)
```

- `GET_POOL` 正常走 `case`，同时 `default` 里再用基号兜一次；
- 真正的活由 `vdrm_pool_list_ioctl(arg, usersz)` 干：按 `usersz` 算出
  "用户结构装得下几条"，只回填这么多条，`copy_to_user` 也只拷这么多字节；
- 于是新旧 App 都能读到池，老 App 只是看到的槽数少一点。

实测（同一台设备、同一个池）：

```text
# 16 条的老结构（3144 字节）
app-struct: 3144 bytes, ioctl=0xcc485607, r=0, count=6   [0] id=1 1920x1080
# 32 条的新结构（6280 字节）
app-struct: 6280 bytes, ioctl=0xd8885607, r=0, count=6   [0] id=1 1920x1080
```

两个不同的 ioctl 号都拿到了池。这条以后就不用再管 App 和驱动谁先更新了。

（第二十五项那个 PREFERRED/DRIVER 位写反的坑同理：都是"不报错、只选错"，
修的时候顺手改成具名常量，别再用裸字面量。）

## 本版本第二十七项：App 侧"强制回收"的超时从 300ms 放到 2s

`native_consumer.c` 里有个看门狗：一帧交给 SF 之后 300ms 还没等到 release 回调，
就当"SF 不会回调了"，强制把槽还给池（`SUB_STUCK_MS`）。这个值太激进：

- SF 的 release 回调挂在「这一帧真正被下一帧替换」之后，屏幕静止、或 SF 这一轮
  没合成这个图层时，回调本来就会晚；
- 300ms 一到就强制 RETURN，可那块 buffer **还在扫描输出** —— 驱动随即又把它
  发给合成器，合成器只重画脏区，屏幕上就留下上一帧的残影（实测是大片灰块）。

现在改成 2000ms，和 fence 超时同档，只当真正的卡死保险。
（另外 `status.sh` 里那个"watcher down"误报也一并修了：它还在按旧的
`input_watch` 名字匹配，而且用了 toybox grep 不认的 BRE `\|` 交替。）

## 本版本第二十八项：横竖——配置决定方向，App 框架转、图层兜底

以前 App 的 Activity 写死 `screenOrientation="landscape"`，而模块的 mode 可以
是竖的（`1080x1920`）。结果是：竖屏档只能在横屏窗口中间缩成一小条（等比
letterbox），既不好看也不叫"自由横竖"。

现在方向由**配置**（模块的 `mode` 参数）决定，三处一起对齐：

### 1) 模块：把配置暴露给 App

新增 ioctl `VDRM_IOCTL_GET_CONF`（`2,'V',0x0d`，`struct vdrm_conf`）：
返回 `mode` 参数（配置值）+ App 上报的窗口尺寸 + `mode_gen`。

和 `GET_INFO` 的区别很关键：`GET_INFO` 的 `width/height` 是 `g_active`
（合成器**已经切过去**的尺寸），改配置到真正切换之间有时间差；方向必须看配置，
否则 App 会晚一步。

### 2) App：图形框架跟着配置转

`MainActivity` 每 500ms 问一次 `Native.nativeGetMode()`（走 `GET_CONF`，
老模块没有这个 ioctl 就退回 `GET_INFO`）：

```java
int want = (h > w) ? SCREEN_ORIENTATION_PORTRAIT : SCREEN_ORIENTATION_LANDSCAPE;
if (want != appliedOrientation) setRequestedOrientation(want);
```

配置是竖的 → Activity 竖屏 → Android 把显示方向也转成竖的 → SurfaceView 变成
`1264x2780`，虚拟屏按原比例铺满。manifest 的 `screenOrientation` 改成
`unspecified`，否则运行时的申请会被 manifest 顶掉。

`configChanges` 里已经有 `orientation|screenSize`，转屏不会重建 Activity，
SurfaceView 的尺寸变化走 `surfaceChanged`。

### 3) App：图层旋转兜底

窗口方向不一定总能跟上配置（系统限制、分屏、或者还没更新 APK 的旧版本）。
这时如果只做 letterbox，竖配置又缩成一条。所以 `sc_set_geometry` 里再补一次：

- `present_rot()`：buffer 方向 ≠ 窗口方向 → 90°（`NATIVE_WINDOW_TRANSFORM_ROT_90`）；
- `present_layout()`：旋转 → 等比缩放 → 居中，几何和触摸映射共用这一个函数，
  所以触点跟着一起转（`touch_map_xy` 做逆变换，按 ROT_90/270 各自还原）。

方向一致时 `rot = 0`，等于什么都不做 —— **框架转到位之后，这层自动不触发**。

运行时可以手动干预（不用重编）：

```sh
setprop debug.vdrm.rotate auto   # 默认：方向不一致才转
setprop debug.vdrm.rotate 0      # 不转，保持 letterbox
setprop debug.vdrm.rotate 90     # 强制转 90（方向不对就换 270）
```

属性只在 App 进程里第一次用到时读一次，改完要重启 App。

实测（`mode=1080x1920`）：

```text
GET_CONF -> conf=1080x1920
niri: Current mode: 1080x1920 @ 120.000 Hz (preferred)
图层按 90° 贴上屏，竖屏画面铺满整块横屏；setprop 改 0 立刻退回 letterbox
```

## 本版本第二十九项：电源键相关的加固（虚拟键盘不该声称自己能出电源键）

起因：部署之后电源键的**按下时长/次数**判定变得可疑。查下来，VDRM 自己从不发
`KEY_POWER`（App 的键表、模块脚本都查过，没有 116），Android 的 InputReader 也
根本打不开我们的节点（实测 `system_server` 只开着真机的 `event0~6/10~12`，
`vdrm-kbd/ptr/touch` 的 7/8/9 一个都没开）。问题在**能力表**：

```text
# 加固前
$ getevent -lp /dev/input/event7 | grep -oE "KEY_POWER2?|KEY_SLEEP|KEY_WAKEUP|..."
KEY_POWER  KEY_POWER2  KEY_SLEEP  KEY_WAKEUP  KEY_SUSPEND  KEY_SYSRQ  KEY_RESTART
```

虚拟键盘当初为了省事，把 0..`VDRM_KEY_MAX` 全声明了 —— 包括电源键。Android 用不到
（节点被藏了），但**内核侧**的键处理器是按设备挂的：`/proc/bus/input/devices` 里
`vdrm-kbd` 的 handlers 就带着 OnePlus 的 `oplus_keyevent_notifier`。多出一台"也声称
能出电源键"的设备，它的电源键手势状态机（长按 / 连击 / 组合键）就可能被算歪。

修法两层：

1. **能力表**：`vdrm_key_system()` 把系统键摘掉 —— `KEY_SYSRQ(99)`、`KEY_POWER(116)`、
   `KEY_SLEEP(142)`、`KEY_WAKEUP(143)`、`KEY_SUSPEND(205)`、`KEY_RFKILL(247)`、
   `KEY_POWER2(0x164)`、`KEY_RESTART(0x198)`；
2. **注入通路**：`vdrm_input_dev_for()` 直接拒收这些码（带限流日志），
   容器里的合成器不可能通过这条 ioctl 合成一次"电源键按下"。

验证：

```text
kbd=event7 系统键:            # 空 = 已经摘掉
```

（配套的 App 侧改动：去掉那个 `SCREEN_BRIGHT | ACQUIRE_CAUSES_WAKEUP` 的唤醒锁，
只保留窗口级 `FLAG_KEEP_SCREEN_ON` —— 前者每次重新申请都会把屏幕点亮，进程被
前台服务拉起时会让已经息屏的手机自己亮起来。）

## 本版本第三十项：修"画面静止后闪一下"——不再把在屏上的 buffer 强制还池

现场抓的（App 侧 logcat，2 分钟窗口）：

```text
10:04:05 W/VDRM: release callback missing slot=8; forcing RETURN
10:04:25 W/VDRM: release callback missing slot=7; forcing RETURN
10:04:36 W/VDRM: release callback missing slot=7; forcing RETURN
...（2 分钟 6 次，全部发生在"画面静止 2 秒后"）
```

机理：SF 的 release 回调只在**这块 buffer 被下一帧替换掉**时才来。画面一静止，
SF 就一直攥着当前这块，回调自然不来；App 的看门狗 2 秒一到就认定"SF 不要了"，
把**还在屏幕上的那块**还回池。驱动随后可能把同一片内存再发给合成器 —— 合成器
往里画，屏上显示的正是那块 → 闪一下 / 花一块。

修法：记住"当前在屏的那一槽"（`g_cur_slot`），看门狗**跳过它**，只处理那些
"不是当前帧、SF 又确实不要了"的残留。当前帧等下一帧替换时 SF 自己回调，那时
才真正归还。

验证（同一台设备，改前/改后同样的测试）：

```text
改前：2 分钟窗口  forcing RETURN 6 次，late release 7 次
改后：60 秒 渲染 25s + 静置 35s   forcing RETURN 0 次
      25 秒 纯静置                forcing RETURN 0 次
```

（同一批还顺手修了另一条会造成"黑一下"的路径：Surface 重建时旧代码会 free 全部
buffer 再 re-forge，新图层在合成器推来第一帧之前是空的。现在 Surface 重建不再
动 buffer，并把上一帧立刻重贴到新图层 —— logcat 里能看到
`re-latch last frame 1080x1920 (slot 8) into new SC`。）

## 本版本第三十一项：fence 超时那条路也不能强还"当前帧"

第三十项修了"release **回调**迟迟不来"那条路，但还有一条同源的：SF 回调来了，
给的是一个 release **fence**（dma-fence），我们 poll 它 2 秒不 signal 就强制归还。

现场（用户复现时的 logcat）：

```text
10:12:58.439 W/VDRM: release fence timeout slot=7; forcing RETURN
```

而静态画面下这个 fence 本来就 signal 不了：HWC 直接拿这块做 overlay 扫描输出，
SF 的回调（连同 fence）要等**下一帧把它换下来**才 signal。2 秒一到就归还，等于
把还在扫描输出的内存还回池 —— 合成器下一秒就可能拿到同一片，屏上闪一下。

修法和第三十项一致：fence 超时分支也跳过 `g_cur_slot`，当前帧继续等下一帧替换；
非当前帧才按超时处理。

验证：修完后 30 秒静置，`forcing RETURN` 0 次、`release fence timeout` 0 次。

## 本版本第三十二项：把"这块已经不在屏上了"的判断交给内核

前两项只是让 App"别瞎猜"，但 App 手上压根没有权威信号：SF 的回调/fence 在静态
画面下本来就不来，超时后无论还还是不还都是猜。这一项把判断搬回内核。

**内核侧（vdrm_pipe_update）**：新帧真正上屏时，把**别的** DISPL 槽直接置
FREE。屏幕上只有一块，新的一块扫描输出了，旧的必然已经不在屏上 —— 这是唯一
权威的"可以安全复用"信号。

**App 侧**：两条超时路径（回调迟迟不来 / release fence 超时）不再归还槽，改成
"放弃跟踪"（`submission_abandon`）：释放自己的引用、关掉 fence，但**不发
RETURN**。槽由内核在上面那个时机回收。

这样就不存在"把还在扫描输出的内存还回池"的路径了：
- 静态画面：当前帧的 fence 一直不来 → 放弃跟踪（槽仍在 DISPL，屏幕上的内容
  不受影响）；下一帧上屏时内核回收。
- 正常播放：fence 按时 signal → 照常 RETURN。

实测：连续渲染 40 秒 + 静置，
```text
slots=6（不泄漏）  frames_unmatched=0  forcing RETURN=0  abandon=0~2
```

## 本版本第三十三项：别再硬投 120Hz（LTPO 屏会闪）

日志证据（用户复现期间，4 分钟窗口）：

```text
RefreshRate: Refresh rate changing from 90.0 to 120.00001   ← 每次 App 获得焦点
RefreshRate: Refresh rate changing from 120.00001 to 90.0   ← 每次 App 失去焦点
SurfaceFlinger: 5 mode changes were performed under the previous policy
SurfaceFlinger: VRR setDesiredActiveMode: renderRate: 60 / 90 / 120   ← 4 分钟 53 次
SDM: DisplayBuiltIn::SetQSyncModeLocked: VRR + update qsync mode   ← 4 分钟 300 次
```

这台是 LTPO 屏：每换一次刷新率/Qsync 档，面板都要重配 gamma/boost，**暗画面
上就是肉眼可见的闪一下**——而容器桌面恰好是暗底。

App 之前硬投 120（`preferredRefreshRate=120`、`Surface.setFrameRate(120)`、
`SurfaceControl.setFrameRate(120, FIXED_SOURCE)`），等于一直把 VRR 策略往 120
拉，和"空闲掉到 60/90"的计时器来回打架，制造了 90↔120 的额外切换。

改成不投票（native 侧已改：`setFrameRate(0, DEFAULT)`），让 SF 按内容实际节奏
自己选。Java 侧那两处投票（窗口 `preferredRefreshRate`、`Surface.setFrameRate`）
也在同一版里去掉。

## 本版本第三十四项：vsync 打拍（参考 anland 的 tick 设计）

anland 的上屏路径（`services/waylandbridge/awl_sc.cpp`）里有三条防抖设计：
不投刷新率、按 vsync 打拍（一 tick 一 latch）、缓冲所有权靠 release fence。
第三条我们已经用"内核在下一帧上屏时回收槽"等价实现了；第一条这一版改掉；
这一项补第二条。

做法：一个独立线程跑 `AChoreographer` 的 frame callback，只记宿主 vsync 的时刻
和周期；渲染线程贴帧前看一眼——离上一次上屏不足一个 vsync 就等到下一个 vsync
（最多等一个周期），期间来的新帧覆盖 pending，等于**同一个 vsync 只上屏一帧**。
拿不到 vsync 信息时完全不打拍，退化成原来的行为。

实测（竖屏 1080x1920、App 在前台、30 秒窗口）：

```text
打拍前：setDesiredActiveMode 10 次 / 30s（60、90、120 混着来）
打拍后：setDesiredActiveMode  3 次 / 30s，面板稳定在 60Hz
帧率：3 秒 165 帧（55fps），没有因为打拍掉帧
```

## 本版本第三十五项：缺 GPU fence（动画花屏的根因）+ 打拍默认关掉

### 缺 fence

驱动里搜不到任何 fence 处理：合成器交上来的 `IN_FENCE_FD`（显式）和 dma-buf 的
隐式 reservation fence，我们一个都没等就扫描输出了。静态画面看不出来（GPU 早画完），
**一放动画每帧都可能扫到画到一半的内容** —— 屏幕上就是一直闪/撕。

补上 `drm_simple_display_pipe_funcs.prepare_fb`：

```c
ret = drm_gem_plane_helper_prepare_fb(&pipe->plane, new_state);  /* 收集 fence */
if (new_state->fence)
        dma_fence_wait_timeout(new_state->fence, false, msecs_to_jiffies(100));
```

（设备内核只导出 `drm_gem_plane_helper_prepare_fb`，`drm_gem_simple_display_pipe_
prepare_fb` 没导出，第一次 insmod 直接 `Unknown symbol`。）

### 打拍默认关掉

`AChoreographer` 在这台机器上给的是 **App 自己的 60Hz 回调**（面板是 120Hz），
于是"同一个 vsync 只上屏一帧"把上屏率压到 ~55fps，贴帧间隔 17~19ms 抖 2ms ——
慢速动画上就是肉眼可见的"一顿一顿"。

实测对比（glmark2 全屏动画、面板 120Hz）：

```text
打拍开：55 fps，间隔 17/18/19ms 混着（每帧隔 2~3 个 vsync，不均匀）
打拍关：115 fps，间隔 8.65ms（稳定）
```

所以默认关（`debug.vdrm.vsync 1` 可再打开实验）。同时 `debug.vdrm.inflight`
可以调 in-flight 上限（默认 4，范围 1..6）。

## 本版本第三十六项：全屏"局部蓝块"的根因与最小解（syncobj + present 驱动）

这一项把前面积累的"局部蓝块/闪屏"彻底收口了。结论是：它不是单点 bug，
而是**三层各自都缺一块**，缺任何一层都会复现。

### 第一层：合成器的 fence 根本没到我们这儿

`drm_ioctl.c` 里这两个 cap 的来源是驱动的 `driver_features`：

```c
case DRM_CAP_SYNCOBJ:          req->value = drm_core_check_feature(dev, DRIVER_SYNCOBJ);
case DRM_CAP_SYNCOBJ_TIMELINE: req->value = drm_core_check_feature(dev, DRIVER_SYNCOBJ_TIMELINE);
```

vdrm 一直是 `DRIVER_GEM | DRIVER_MODESET | DRIVER_ATOMIC`，于是：

```text
DRM_CAP_SYNCOBJ=0, DRM_CAP_SYNCOBJ_TIMELINE=0
   → smithay（niri 的底层）判定"不支持显式同步"，提交时不带 IN_FENCE_FD
   → 高通 kgsl 又不做隐式同步
   → 整条链零同步
```

设备实测：那一刻 `fence_waits` 恒为 **0**（`prepare_fb` 里的 `new_state->fence`
永远是 NULL），而 App 一 latch、SF/HWC 立刻扫——GPU 没画完就扫到半成品。

修法一行（syncobj 是 DRM 核心实现，与具体驱动无关，虚拟卡声明它是正当的）：

```c
#define VDRM_DRIVER_FEATURES (DRIVER_GEM | DRIVER_MODESET | DRIVER_ATOMIC | \
                              DRIVER_SYNCOBJ | DRIVER_SYNCOBJ_TIMELINE)
```

改完 `fence_waits` 立刻与 `frames_pushed` 1:1（实测 82334/82334，超时 0）。

### 第二层：flip-done 说的是"提交完成"，不是"屏幕显示完"

合成器的 buffer 循环是 **flip-done 即空闲**。我们把 flip-done 挂在 hrtimer 的
假 vblank 上 —— 它在提交被接受时就发了。可真实消费端还有一层：

```text
niri → vdrm（假显示器）→ App → SurfaceFlinger → HWC/DPU → 面板
                  ↑ 我们在这里就说"翻页完成"
```

SF 把 buffer 锁进 overlay 平面后要等下一次合成才 release（三缓冲下再多占
1–2 个 vsync）。于是 niri 提前认为这块空了，在 DPU 还在扫的时候覆写 —— 
**`frames_reuse` 与 `frames_pushed` 同步增长**，屏幕上就是局部蓝块，而录屏
走另一条合成路径、读的是覆盖之后的像素，所以**永远录不到**。

修法：新增 `VDRM_IOCTL_PRESENTED`，App 在 SurfaceFlinger 的 present 时刻把
槽 id 报回来；flip-done 改由它驱动（present 之后再过一个 vblank 完成，这样
覆写点晚于 DPU 读完这一帧）。同时保留两条更保守的完成路径：

1. App `RETURN`（= SF release，最权威，可能更早）；
2. 兜底超时（present/return 都不来时仍能跑）。

### 第三层：中间试过的两条弯路（已删除，记录在此避免重走）

- **拷贝**（App 每帧把池槽 blit 进自己的 AHB）：能解耦，但 CPU 拷贝要
  14 MB/帧，且如果拷到的正是"正在被写"的那帧，照样闪。
- **gating**（flip-done 等 App RETURN 再放行）：零拷贝下 App 要等 SF release
  才 RETURN，而 SF 常常 >100ms 不释放 → 整条链死锁到 ~9fps。

两者都只是把问题挪了个位置。真正的解是让 flip-done 反映**真实上屏时刻**。

### 实测（1264x2780@120，glmark2 全屏动画 + 来回切焦点）

| 指标 | 修之前 | 修之后 |
|---|---|---|
| 局部蓝块 | 每来一个新窗口闪一下 | **没有**（高负载下也不闪） |
| `frames_reuse` 增量 | 与 `frames_pushed` 同步涨 | **0** |
| `fence_waits` | 0（根本没有 fence） | 与 `frames_pushed` 1:1 |
| `flip_present_done` | — | 与 `frames_pushed` 1:1 |
| 帧率 | 115fps（但闪） | ~39fps（被 present + 1 vblank 的安全边界限住） |

帧率下降是这条链路的固有代价：present 之后必须等一个 vblank 才允许合成器
复用显存。想更激进可以把这个边界缩到半个周期，但要用 `frames_reuse` 和肉眼
一起确认。

### 同时删掉的东西

- App：`copy_blit.c` 整条拷贝路径（EGL blit + CPU memcpy 兜底）；
- App：`debug.vdrm.copy` / `vdrm_copy` 文件两个开关；
- App：`debug.vdrm.bufalpha` / `debug.vdrm.shrink` 两个实验开关（都试过，HWC 都吃得下，没能逼出 GPU 合成）；
- 驱动：`flip_wait_release_ms` 参数（gating 语义已由 present 驱动取代）。

---

## 本版本第三十七项：面板尺寸从"App 上报"改成"mkpool 自己查"

### 问题

`modes` 里那两档"全面屏"分辨率（如 `1264x2780 / 2780x1264`）来自
`VDRM_IOCTL_SET_SCREEN`，而以前只有 App 会调它，上报的还是**窗口**尺寸。
于是出现两个后果：

1. **顺序依赖**：App 不起，`modes` 只有 `1080x1920 / 1920x1080`；这时写
   `mode=1264x2780`，`vdrm_modes_rebuild()` 会把这一档从列表里删掉
   （`vdrm_res_advertised()` 不认它），合成器只能退回 16:9 —— 表现是黑屏
   或者画面被压在中间一条。
2. **窗口尺寸污染**：分屏/小窗时 App 上报的是窗口大小，一上报就把面板那两档
   挤掉，合成器连切都切不回去。

根子上是搞错了语义：**"这块屏有多大"和"App 的窗口有多大"不是一件事**，
前者是面板属性，后者只影响 letterbox 几何。

### 改法

- `vdrm_mkpool`：建池前自己查面板尺寸（`wm size` 的输出，有 `Override size`
  就用它，否则用 `Physical size`），然后 `SET_SCREEN` 告诉模块。
  `--no-screen` 可以跳过。
- `vdrm_mkpool` 建池的目标尺寸改成直接读 **`mode` 参数**，不再用 `GET_INFO`
  的 `width/height`（那是 `g_active`，代表"合成器此刻真正在用的尺寸"）。
  这样运行中切分辨率时，池会先按新尺寸补齐，而 App 的转屏仍然只跟
  `g_active` 走 —— 合成器真换了才转。

  > 试过在 `vdrm_mode_apply()` 里同步 `g_active`，结果是"参数一写 App 就转屏、
  > 合成器还停在旧尺寸"，屏幕上就是"App 竖着、画面横着"。已回退。
- App：删掉 `SET_SCREEN` 调用。窗口尺寸继续走 `ANativeWindow_getWidth/Height`
  做几何，不再影响 `modes`。

### 结果

启动顺序从「App → mkpool → 合成器」变成「模块 → mkpool → 合成器 (+App)」。

另外运行中切分辨率也变成两步（合成器会跟 hotplug 自己换档，实测 1 秒内切完，
不用 `niri msg output`）：

```
insmod vdrm-*.ko mode=1264x2780
./vdrm_mkpool --reset        # 不跑 App 也能把 modes/池按 1264x2780 建对
niri                         # picks 1264x2780
# App 随便什么时候起
```

---

## 本版本第三十八项：切分辨率改成 prepare / commit 两段（热插拔时序）

### 问题

`echo WxH > .../mode` 以前立刻更新 EDID 并发 hotplug。合成器（niri 实测）
几百毫秒内就换档了，而新尺寸的槽要等 `mkpool` 跑完才进池 —— 这段窗口里
合成器拿不到 buffer：

```
niri: error changing mode: Failed to create dumb buffer ...
      No space left on device
niri: ERROR connector mode list missing current mode
```

表现就是"App 已经转屏了，画面还是上一档的"。

顺带挖出第二个 bug：`mode_slots` 是按 `g_active`（合成器此刻在用的尺寸）数的，
而 `mkpool` 用它判断"这一档还缺几个槽"。参数刚改、合成器还没切时，数出来的是
旧档的槽数，mkpool 就以为池够、一个槽都不建 → 合成器真去要就 ENOSPC。

### 改法

- `mode` 参数写入 = **prepare**：只记目标尺寸，不碰 EDID、不发 hotplug。
- 新增只写参数 `commit`（`echo 1 > .../commit`）= **commit**：更新 EDID + 发
  hotplug。没有 pending 时是空操作。
- `vdrm_mkpool --mode WxH`：内部按 `prepare → 建槽 → commit` 走完，对外一条命令。
- `mode_slots` 改成按**目标尺寸**（`g_mode_w/h`）统计，`mkpool` 的判断才准。

### 结果

时序与合成器无关：合成器收到热插拔时槽一定已在池里。跟 hotplug 的
（niri / KWin / Hyprland）自己换档，不跟的用各自工具切也一样安全。

实测（同一次 niri 会话，App 不重启）：

| 操作 | 效果 |
|---|---|
| `vdrm_mkpool --mode 2780x1264` | 建 6 槽 → commit → niri 自己切到横屏 |
| `vdrm_mkpool --mode 1264x2780` | 池已有 6 槽（不重建）→ commit → niri 切回竖屏 |
| `frames_reuse` 增量 | 0 |
| niri 日志 | 无 `Failed to create dumb buffer` |

---

## 本版本第三十九项：清理（删掉不再需要的东西）

### 内核

- **`render_node` 参数删除**：render 节点永远暴露。只有 primary 节点、没有
  render 节点的设备会让一部分合成器/库直接放弃硬件路径 —— 标准做法就是成对，
  不需要 A/B 开关。
- 统计参数（`frames_*` / `fence_*` / `st_*` / `cap_*`）全部保留，排障还在用。

### App（native_consumer.c）

- 删 6 个 0 调用点的死函数：`sc_probe`、`vdrm_unregister`、`ahb_checksum`、
  `vdrm_input`、`collect_buffers`、`buf_slot`。
- **删整条 shm 通道**：`struct vdr2_frame_info`、memfd + mmap + ftruncate、
  `VDRM_IOCTL_SET_SHM`。内核侧从来没有 0x05 这个 ioctl，这行调用一直返回
  ENOTTY，mmap 出来的 4KB 也没人读 —— 帧信息早改由事件 + `PRESENTED` 承载。
- **删 vsync 打拍子系统**：Choreographer 线程 + `vsync_cb`/`vsync_gate`/
  `vsync_enabled` + `debug.vdrm.vsync`。上屏节拍现在由"present 回调 → 驱动算
  flip-done"这条真实链路负责，等价于硬件节拍器，App 自己打拍只会压帧率
  （实测会掉到 ~55fps）。
- 删死宏 `VDRM_IOCTL_SET_SCREEN`、`VDRM_IOCTL_UNREGISTER` 和过期注释
  （`debug.vdrm.bufalpha` / `shrink` 两个实验早就删了，只剩说明）。
- 文件头注释重写：原来写的还是 kpatch/KPM ctl0 那套架构，早就不存在了。

### App（Java / Manifest）

- 删 `WAKE_LOCK` 权限：Java/native 里没有任何 WakeLock（当年那个唤醒锁已删）。

### 仓库

- 15 个内核构建产物（`*.o`、`.*.cmd`、`*.mod`、`Module.symvers`、
  `modules.order`）从 git 移除，并补进 `.gitignore`。
- `test/crtc_probe`、`test/vdrm_pointer_anim` 两个编译好的二进制移出 git（`.c` 保留）。
- `dist/`（12 个旧发布 zip）、`AUDIT.md`（早期审计，结论多条已过时）删除。

### 验证

重载新 `.ko` + 热替换新 `.so` 之后：

- `renderD129` 仍在，`/sys/module/vdrm/parameters/` 里没有 `render_node`；
- niri 起在 1264x2780，App 前台，`frames_pushed` 正常增长；
- `vdrm_mkpool --mode 2780x1264` → `--mode 1264x2780` 来回切，niri 都自己跟上，
  日志里 `error changing mode` / `No space left` 均为 0。

---

## 本版本第四十项：切分辨率后丢弃"旧尺寸的帧"（旋转反向的最后一环）

第三十八项把池的时序修好了（合成器换档时槽一定在），第三十九项之前又加了
"模式变了先把图层藏起来"。但还漏了一种情况：**藏起来之后，事件里下一帧指向的
可能仍是旧尺寸的槽**（合成器还没按新尺寸重画）。这种帧被贴上去时，几何是按
新窗口/新 mode 算的，`present_rot` 于是多转一个 90° —— 表现就是"旋转又错了/反向"。

改法：`sc_latch` 里先 describe 这块 AHB，**尺寸与当前 mode 不一致就丢掉**
（返回 false，调用方把槽 RETURN 回去），保持图层隐藏，等尺寸对上的那一帧。

实测：横 → 竖、竖 → 横来回切，画面方向正确；`logcat -s VDRM:W` 里能看到
`drop stale WxH (mode WxH)`，没有一次转错。

---

## 本版本第四十一项：旋转的最后三处（vblank 预热 + 方向跟随真实档位 + 窗口变了重贴）

用户实测反馈："只要不出帧就一直贴上一帧" —— 切分辨率时合成器常常只出一帧
（之后没 damage 了），而这一帧经常在系统还没转完屏时就到了。三个改法：

1. **内核：切档时给 vblank 预热 3 个 tick**（`g_vblank_kick_ticks`）。
   `commit` 之后合成器一收到 hotplug 就会重新提交，这几拍让挂起的 flip-done
   及时完成，它马上再出一帧 —— 图层能及时换成新尺寸的画面。预热用完自动停，
   平时网格仍然是停的（`vblank_running=0`）。
2. **App：转屏跟随"真实生效尺寸"**。`nativeGetMode()` 原来优先用 `GET_CONF`
   （= mode 参数 = 下次的目标尺寸），于是 mkpool prepare 的那一刻 App 就转屏，
   比合成器换档还早。改成优先 `GET_INFO` 的 `width/height`（g_active，只有
   `pipe_enable` / `dumb_create` 会改它），GET_CONF 只作兜底。
3. **App：窗口尺寸变了就把最后一帧按新几何重贴**（`nativeStart` 的纯 resize 分支）。
   切档时那一帧往往按"窗口还没转"算了 rot=90°，之后再没新帧改回来，屏幕就
   一直转错方向。现在窗口一转完就 `sc_show_last()` 重贴一次（尺寸与当前 mode
   一致才贴，避免贴旧尺寸的帧）。

### 实测

竖直 ↔ 横屏来回切（App 前台，合成器不重启）：

| 状态 | 截图尺寸 | 画面方向 |
|---|---|---|
| 竖屏 | 1264x2780 | 终端标题/文本水平，正常 |
| 横屏 | 2780x1264 | 正常 |
| 再回竖屏 | 1264x2780 | 正常 |

`vblank_ticks` 每次切档 +3（预热），之后 `vblank_running` 回到 0。
