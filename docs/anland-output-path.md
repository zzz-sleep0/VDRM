# anland 的上屏路径分析（对照 VDRM）

源码：https://github.com/superturtleee/anland → 本地 `/root/anland-src`
（`services/waylandbridge/awl_sc.cpp` 是核心，1252 行，头部 90 行就是设计说明）

## 它防闪的四条做法

1. **从不投票刷新率**
   全仓库没有 `setFrameRate` / `preferredRefreshRate` / `FRAME_RATE_COMPATIBILITY`。
   帧率完全交给 SF 的 VRR 策略，不跟系统/其他窗口抢。

2. **按 vsync 打拍（AChoreographer tick）**
   一个渲染线程跑 `AChoreographer` 循环：
   - 每个 tick，每个可见图层最多 latch 一次；
   - 两个 tick 之间有 commit 时允许"off-tick"额外过一趟（降延迟），
     但 off-tick 只 latch、不发 frame_done；
   - frame_done 在 **tick** 上发给客户端（"a callback per vsync is the contract"），
     而不是等 SF 的 OnComplete（那会晚一个 vsync，实测 60Hz 上只剩 29fps）。

3. **缓冲的所有权靠 release fence，不靠超时**
   `setBufferWithRelease` 的回调带 SF 的 release fence → 元素带着 fence 回到它自己的
   队列 → 生产者取用前先等这个 fence。
   代码里没有任何"等不到就强行收回"的路径；SF 还攥着的缓冲永远不会被别人拿去画。
   （注释原文：*whatever is latched on an SC stays referenced until SF is done with it*）

4. **跑太快的生产者改成拷贝（EGL 模式）**
   零拷贝扫描输出时 SF 会攥住 3 块、队列头再 1 块；客户端若持续超速，
   它就没有空 buffer 了（一行 vsync 只出 1 帧）。anland 的做法是给这种客户端
   走 EGL 拷贝路径，而不是让它饿死或复用还在屏上的缓冲。

另外还有一条和我们相关的：**图层移除 = hide + reparent(NULL)**，引用要等这笔
transaction apply 之后才 drop —— 否则"释放了但没摘掉"的图层会一直留着最后一帧。

## 和我们现在的差异

| 能力 | anland | VDRM 现状 |
|---|---|---|
| 刷新率投票 | 从不投 | 之前硬投 120（native 已改成 0，Java 两处待改） |
| vsync 打拍 | AChoreographer tick，一 tick 一 latch | 来一帧就 latch，只靠 3 个 in-flight 上限兜着 |
| 缓冲归还 | release fence 链，无超时 | 内核在"下一帧上屏"时回收槽（等价保证，已实现） |
| 图层移除 | hide+reparent，apply 后 drop | 一样（`sc_remove`） |

## 结论

- 我们**没有**"超时强还缓冲"的路径了，和 anland 的所有权模型等价；
- 还差的只有**刷新率投票**（改掉）和**vsync 打拍**（可选，降抖动）。
- LTPO 面板每换一次刷新率/Qsync 档都会重配 gamma，暗画面上就是闪一下 ——
  所以"不投票 + 不打乱节奏"这两条是最关键的。
