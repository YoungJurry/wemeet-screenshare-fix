# 共享选项 ⓘ 提示气泡：布局回调的重入生命周期

适用范围：完整 SHA-256 校验通过的 `wemeet-bin 3.26.10.401-5`；
Linux x86-64 / GNU libstdc++。此守卫与 PipeWire Guard 独立。
仓库不存放腾讯 ELF、截图、core 或私人日志。

## 原始故障与证据

共享选择窗口中“同时共享电脑声音”和“视频流畅度优先”右侧的 ⓘ，
在悬停创建说明气泡时触发主线程 SIGSEGV。core 的调用路径经过
`NXQtViewRoot::resizeEvent`、`AdjustSize`、`Placeable::PlaceAt`，
最终在 framework `+0x9e99cc` 解引用非法的 NaturalView 指针。

关键不是用户手动调整主窗口，也不是 PipeWire 帧复制：
framework `+0x908460` 的放置闭包执行逻辑为：

```text
child = closure[1]
child.PlaceChildren(x, y)           # 重入并替换正在执行的 std::function
node = closure[0]                  # 按旧闭包格式解释已覆写的缓冲区
natural_view = node->field_at_0x70
if natural_view:
    notify(natural_view, child)
```

原闭包的两个普通指针 `[node, child]` 位于 `std::function` 的小对象
缓冲区。core 的栈仍在执行旧 invoker `+0x9082e0` / 函数体
`+0x908460`，但 function 的 manager / invoker 已变成
`+0x907fa0` / `+0x907f40`。新 functor 含一个非平凡的
`shared_ptr<Placeable>`，存储形式变成指向堆上 holder 的指针。

旧闭包把 holder 当作 node；holder `+0x70` 的内容恰为
`double 32.0` 的位模式 `0x4040000000000000`，被误当成
NaturalView 指针并解引用。这直接支持“执行中的闭包存储被替换”的
解释，比笼统的 node use-after-free 更具体。

## 已否决的首版

首版在旧闭包内部保存 NaturalView / child 的裸指针快照，改写了
52 字节指令。它通过指定的离线覆写模型，并在真实窗口中恢复了说明
气泡和共享操作，但两轮正常退出时又检测到 SIGABRT：一次在后台线程
报告 `corrupted size vs. prev_size`，一次在 `xcast_shutdown` 的
free 路径报告堆损坏。

关闭首版且避免悬停的一轮共享/退出没有新 core。这不足以排除 SDK
自身的退出竞态，也不能证明某个具体写入就是根因，但已足以否决“修复
验证完成”的结论。**首版指令补丁不在当前实现中，不应使用或发布。**
只保存裸指针不等于保持执行中整个回调的存储/所有权有效。

## 当前实现：复制完整回调

`src/layout_guard.cpp` 提供独立 `libwemeet_layout_guard.so`，拦截经审核的
weak ELF 符号 `_ZNKSt8functionIFvddEEclEdd`
（`std::function<void(double,double)>::operator()`）。

只有同时满足以下条件才建立副本：

1. 启动器启用 `WEMEET_TOOLTIP_FIX=1`；
2. `dladdr` 确认调用者来自 `/usr/lib/wemeet/libwemeet_framework.so`；
3. 调用返回地址严格等于该文件 `+0x917f45`，
   即 `Placeable::PlaceAt` 的已审核 PLT 调用点。

```cpp
std::function<void(double, double)> snapshot(*source);
invoke_original_functor(snapshot, x, y);
```

真正的 copy constructor 调用源 functor 的 manager，既隔离小对象存储，
也保留拥有型捕获。重入可以替换源 function，但正在执行的副本仍有效。
返回或抛出异常时，副本按 RAII 正常析构。

调用不经过被拦截的公开 `operator()`，以免递归，而是读取审核过的 GNU
布局中的 invoker，使用精确签名
`void(const std::_Any_data&, double&&, double&&)` 调用原 functor。
编译期验证对象大小 32、invoker 偏移 24，并拒绝非 GNU libstdc++ 或非
x86-64 构建。其他调用点和关闭开关时不复制，保留直接调用行为；
空 function 仍抛出 `std::bad_function_call`，原异常不捕获、不吞掉。

目标调用点包含所有 `Placeable::PlaceAt` 回调，不仅是一个 tooltip
闭包。目标 mutable functor 的内部状态修改会发生在副本上；这是必须
记录的语义边界，不能泛化为任何 std::function 使用场景的修复。
复制回调也不自动延长任意裸指针所指对象的寿命。

## 版本、隔离与开关

原始 framework 完整 SHA-256：

```text
2118f17803de4fb8e02b44d273b6301f206e893f6d1024495b605648c510ed2a
```

`tools/stage_tooltip.py` 验证整文件 hash，写入一个新的、**逐字节不变**
的副本。禁止原地、硬链接、已有输出和输出符号链接覆写，无环境/CLI
哈希豁免。启动器再次验证副本 hash，以只读挂载固定该进程使用的
framework。即使关闭 UI 守卫，主机 framework 也必须通过版本校验。

只在腾讯会议 bubblewrap 进程中加入两个 Guard，保持 preload 顺序：

```text
libwemeetwrap.so : libwemeet_layout_guard.so : libwemeet_pw_guard.so
```

Tencent wrapper 保持首位。UI 守卫只导出上述一个 operator 符号，不新增
PipeWire / Qt / allocator hook，不修改腾讯 UI 指令、不关闭气泡。Hunk 0
屏幕共享模块副本仍保持原有隔离补丁，主机安装文件不变，退出时删除
运行时副本。

```sh
# 正常启动（两个 Guard 默认启用）
./tools/run-guarded.sh

# 受控关闭 UI 守卫；仍保留 PipeWire Guard，不要故意触发未修复的 ⓘ
WEMEET_TOOLTIP_FIX=0 ./tools/run-guarded.sh

# 只验证版本与挂载，不启动会议
WEMEET_PW_GUARD_VALIDATE_ONLY=1 ./tools/run-guarded.sh
```

checkout 优先使用当前源码及 staging 工具；开发编译目录可用
`WEMEET_PW_GUARD_BUILD_DIR` 设置。安装包使用系统库目录。
框架加载路径不匹配时守卫不触发，而不是猜测另一个调用点。

## 离线测试与真实验证

```sh
make test-tooltip
make test
```

离线测试不加载腾讯库、不启动会议、不联网：

- 合成 staging 的未知 hash、原地/链接/已存在输出拒绝和精确复制；
- 2,000 次双指针小对象闭包覆写，保持原 node / child 与坐标；
- 2,000 次拥有型捕获替换：执行期间存活，返回后正常释放；
- 非目标/关闭模式透传、空回调异常、抛出后的 RAII 清理；
- 独立 LD_PRELOAD 集成测试，以**测试进程内** mock dladdr 验证调用者
  路径/偏移门禁和实际导出符号的捕获/异常行为，不放宽生产库门禁；
- 原有 PipeWire mock、生命周期与并发 destroy 测试。

2026-10-12 本机真实验证（用户操作，与离线测试分开）：

| 方案 | 悬停 / 共享开始停止 | 正常退出后的记录 |
| --- | --- | --- |
| 首版裸指针快照（已撤销） | 表面正常 | 两轮 SIGABRT，否决 |
| 关闭 UI 修复、避免悬停 | 共享正常 | 对照一轮无新 core |
| 当前完整回调副本 | 两个气泡和共享正常 | 一轮正常退出，无新 core |

当前一轮记录：7,134 次布局回调快照；共享收到 205 帧，PipeWire Guard
复制 215 次、copy_failures=0。另有 25 个独立生命周期测试进程及
ASan/UBSan（含泄漏检测）通过。最后的精确 invoker 类型整理后，
生成的 `.text` 与真实测试时逐字节一致。

这些是限定版本、限定环境的短期证据，**不是长期稳定性保证**。仍需更长
会议、反复重开选择窗口、更多布局变化和退出周期测试。源码与文档可
发布；腾讯二进制、core、截图和私人日志不得加入仓库。
