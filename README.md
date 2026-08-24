# 腾讯会议 Wayland 屏幕共享修复（实验性）

这是面向 Hyprland/XDPH 的腾讯会议 `3.26.10.401-5` 屏幕共享实验性修复。
仓库不包含腾讯二进制、编译产物、core dump 或私人日志。

> **严格版本绑定：** 补丁和启动器仅支持 Arch/CachyOS 的
> `wemeet-bin 3.26.10.401-5`。`tools/stage_hunk0.py` 与
> `tools/run-guarded.sh` 内置了经过验证的 SHA-256，遇到任何其他二进制都会
> 拒绝执行。腾讯会议升级后必须重新审计所有偏移，不能复用现有补丁。

## 问题说明

修复 Portal 调用顺序后，腾讯会议仍可能在 PipeWire stream 已释放或移除
buffer 后继续持有对应 SHM 帧指针。实测故障包括：

- 对已经解除映射的 `14,745,600` 字节 SHM 帧执行延迟 `rep movsb`；
- `pw_stream_queue_buffer()+41` 解引用已经失效的内部 buffer；
- stream 销毁和重建期间，XDPH 报告 `Out of buffers`。

此前上游 `straight` Hook 会先用 `msync()` 检查映射，再复制帧。这仍然存在
TOCTOU 竞态：检查完成后，映射可能在复制过程中被另一线程解除。

## 完整修复由什么组成

当前可工作的实验方案分为三层：

1. **Portal 调用顺序——上游 Hunk 0**
   让腾讯会议等待异步 `SelectSources` 响应完成后再调用 `Start`，修复
   `Sources not selected`。
2. **CPU 可读帧——XDPH `force_shm = 1`**
   避免仅 DMA-BUF 的帧进入腾讯会议延迟 CPU 复制路径。没有启用 RGB/BGR
   颜色交换。
3. **生命周期安全——本仓库的 PipeWire Guard**
   在 PipeWire 仍保证帧有效时创建快照，保持镜像缓冲区长期映射，在 queue
   前恢复生产者原始指针，并拒绝 buffer 移除或 stream 销毁后的迟到 queue。

Hunk 0、`straight` 和 `force_shm` 方案均来自上游。本仓库新增的工作是独立
实现的 PipeWire buffer 生命周期守卫、mock/压力测试、严格版本校验、隔离
启动器及相关文档。

## PipeWire Guard 设计

腾讯会议屏幕共享模块自行打开 `libpipewire-0.3.so.0`，并通过 `dlsym()`
解析 API。因此 Guard 拦截 `dlsym()`，且只替换以下函数：

- `pw_stream_new`
- `pw_stream_destroy`
- `pw_stream_add_listener`
- `pw_stream_connect`
- `pw_stream_disconnect`
- `pw_stream_dequeue_buffer`
- `pw_stream_queue_buffer`

每次 dequeue 后，Guard 会在 PipeWire buffer 仍归客户端所有时复制有效的
`spa_chunk` 区间。它将 `spa_data.data` 临时替换为八个长期匿名镜像映射之一，
并在归还 buffer 前恢复 PipeWire 原始指针。镜像在进程结束前不会解除映射，
因此腾讯会议的延迟读取仍能访问有效内存。

Guard 包装 `add_buffer`、`remove_buffer`、`process`、disconnect 和 destroy
事件以跟踪生命周期。若完成回调尝试 queue 已移除或已销毁的 buffer，Guard
返回 `-EPIPE`，不会调用真正的 `pw_stream_queue_buffer()`。

只有设置 `WEMEET_PW_GUARD=1` 才会启用替换；仅加载动态库时不会替换目标
PipeWire API。

## 环境要求

- x86-64 Arch/CachyOS、Hyprland 和 `xdg-desktop-portal-hyprland`；
- 与内置哈希完全一致的 `wemeet-bin 3.26.10.401-5`；
- PipeWire 开发头文件、C 编译器、`make`、`pkg-config`、Python 3 和
  bubblewrap；
- `desktop-file-utils` 仅用于校验桌面入口和刷新应用菜单缓存。

## 构建与测试

编译产物和测试二进制只会写入 `/tmp`：

```sh
make clean test
```

mock PipeWire 测试覆盖：

- 帧快照和原始指针恢复；
- 原始映射失效后，镜像仍然可读；
- `remove_buffer`、disconnect 和 destroy 后拒绝迟到 queue；
- 2,000 次正常 process/dequeue/queue 循环；
- process 与 destroy 并发时，destroy 等待活动回调且迟到 queue 被拒绝；
- Guard 关闭时保持透传行为。

额外本地检查包括 GCC `-fanalyzer`、ASan/UBSan、50 个独立测试进程，以及
300 帧 `2560×1440×4` 全尺寸压力测试。ThreadSanitizer 无法与自定义
`dlsym()` interposer 一起初始化，即使 Guard 关闭也会失败，因此该路径使用
确定性的并发 destroy 测试覆盖。

## 安装与启动

本动态库不能单独修复 Portal 调用顺序，也不能让 DMA-BUF 变为 CPU 可读。
完整实验方案要求：Hunk 0、XDPH SHM、原版 `libxcast.so`，并且不能同时使用
`straight` 或颜色交换 Hook。

将以下配置合并到 `~/.config/hypr/xdph.conf`。如果文件已有其他设置，不要
直接覆盖：

```ini
screencopy {
    force_shm = 1
}
```

重启 XDPH，然后执行测试：

```sh
systemctl --user restart xdg-desktop-portal-hyprland.service
make clean test
```

从仓库目录启动修复版腾讯会议：

```sh
./tools/run-guarded.sh
```

启动器会检查 XDPH 设置和三个腾讯会议文件的哈希。它在 `/tmp` 中创建经过
验证的 Hunk 0 副本，并且只在腾讯会议 bubblewrap mount namespace 内覆盖
该文件，因此主机软件包保持原版。系统重启导致 `/tmp` 清空后，启动器会自动
重新编译 Guard。

普通 `wemeet` 命令仍会启动未修复的官方版本。腾讯自带的
`libwemeetwrap.so` 必须位于 `LD_PRELOAD` 首位；若 Guard 排在它前面，会
破坏 wrapper 的 `RTLD_NEXT` 查找，导致腾讯会议只有后台进程而不显示窗口。

可以安装一个独立应用菜单入口，不会覆盖官方入口：

```sh
./tools/install-desktop-entry.sh
```

菜单中会显示为 **腾讯会议（Wayland 修复版）**。删除方法：

```sh
rm -f ~/.local/share/applications/wemeet-wayland-guard.desktop
update-desktop-database ~/.local/share/applications
```

## 上游来源与归属

Hunk 0 的偏移和补丁字节来自：

[`GodKeawa/wemeet-bin-wayland-screenshare-fix`](https://github.com/GodKeawa/wemeet-bin-wayland-screenshare-fix)

本仓库没有将 Hunk 0 描述为自行发明，也不分发腾讯会议二进制。

## 当前实测状态

Guard 已通过合成测试，并在重启前后完成两轮真实屏幕共享测试。第二轮成功
接收并编码超过 700 帧 `1920×1080` 画面，停止共享正常，且没有产生 Wemeet
core dump。第一轮初始协商期间出现两次瞬时 XDPH `Out of buffers`，随后自动
恢复并正常共享。

这些结果是有希望的开发阶段证据，但还不能视为长期稳定性保证。欢迎在严格
匹配版本的前提下测试并提交不包含私人数据的日志摘要或问题报告。
