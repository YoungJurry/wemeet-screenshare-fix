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
2. **CPU 可读帧——PipeWire 标准 SHM 协商**
   腾讯会议不发布 `SPA_FORMAT_VIDEO_modifier`，因此不能匹配 XDPH 带 mandatory
   modifier 的 DMA-BUF 候选，只能匹配其 SHM fallback。真实 A/B 测试确认最终
   buffer 为 `SPA_DATA_MemFd`，无需设置全局 `force_shm`。没有启用 RGB/BGR
   颜色交换。
3. **生命周期安全——本仓库的 PipeWire Guard**
   在 PipeWire 仍保证帧有效时创建快照，保持镜像缓冲区长期映射，在 queue
   前恢复生产者原始指针，并拒绝 buffer 移除或 stream 销毁后的迟到 queue。

Hunk 0、`straight` 和最初用于排查的 `force_shm` 方案均来自上游。本仓库
新增的工作是独立实现的 PipeWire buffer 生命周期守卫、mock/压力测试、严格
版本校验、隔离启动器及相关文档。后续协议分析和真实 A/B 测试证明腾讯会议
默认已经协商 SHM，因此正式方案不再要求全局 `force_shm`。

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

## Arch/CachyOS 软件包安装

仓库在 [`packaging/arch/`](packaging/arch/) 中提供固定版本的 `PKGBUILD`。
它只下载并构建本仓库的 MIT 源码，不包含或重新分发腾讯会议二进制。

先安装 `base-devel`，并确保已安装的腾讯会议版本严格匹配：

```sh
pacman -Q wemeet-bin
# 必须输出：wemeet-bin 3.26.10.401-5
```

`wemeet-bin` 是 AUR 依赖，若尚未安装，需要先通过 AUR helper 安装；随后构建
并安装本修复包：

```sh
git clone https://github.com/YoungJurry/wemeet-screenshare-fix.git
cd wemeet-screenshare-fix/packaging/arch
makepkg -si
```

安装后可从应用菜单启动 **腾讯会议（Wayland 修复版）**，也可以运行：

```sh
wemeet-wayland-guard
```

官方腾讯会议入口保持不变。卸载修复包不会修改或删除腾讯会议文件及用户
XDPH 配置：

```sh
sudo pacman -Rns wemeet-screenshare-fix
```

在 AUR 包正式发布前，这种本地 `makepkg -si` 安装方式不会被 `yay -Syu`
自动升级；更新时需要重新拉取本仓库并构建。

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
完整实验方案要求：Hunk 0、PipeWire 协商得到 `SPA_DATA_MemFd`、原版
`libxcast.so`，并且不能同时使用 `straight` 或颜色交换 Hook。

不需要创建或修改 `~/.config/hypr/xdph.conf`。XDPH 默认同时发布 DMA-BUF
候选和 SHM fallback；腾讯会议的无 modifier 格式会自动选择后者。全局
`force_shm = 1` 会影响其他 Portal 应用，不属于本修复的安装步骤。

先执行测试：

```sh
make clean test
```

从仓库目录启动修复版腾讯会议：

```sh
./tools/run-guarded.sh
```

启动器会检查已安装包必须为 `wemeet-bin 3.26.10.401-5`，并验证三个腾讯
会议文件的哈希。它在 `$XDG_RUNTIME_DIR` 的唯一临时目录中创建经过验证的
Hunk 0 副本，并且只在腾讯会议 bubblewrap mount namespace 内覆盖该文件，
因此主机软件包保持原版。腾讯会议退出后，启动器会删除该临时副本。

从源码运行时，Guard 编译产物仍只写入 `/tmp`，重启后由启动器自动重建。
系统包则可执行：

```sh
make BUILD_DIR="$PWD/build" DESTDIR="$pkgdir" install
```

将 Guard、Hunk 0 staging 工具、系统启动器和独立 desktop 安装到标准 `/usr`
路径；该安装目标同样不会修改腾讯会议文件或任何用户配置。

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

## 当前实测状态

Guard 已通过合成测试，并在重启前后完成两轮真实屏幕共享测试。第二轮成功
接收并编码超过 700 帧 `1920×1080` 画面，停止共享正常，且没有产生 Wemeet
core dump。第一轮初始协商期间出现两次瞬时 XDPH `Out of buffers`，随后自动
恢复并正常共享。

另一次关闭全局 `force_shm` 的真实 A/B 测试连续完成两轮开始和停止共享。
Guard 在实际 `add_buffer` 事件中确认 `n_datas=1`、`spa_data.type=2`，即
`SPA_DATA_MemFd`；XDPH 同时记录 `Building modifiers for dma` 和随后实际请求
`wl_shm` buffer。这证明前一条日志只是发布 DMA 候选，并不表示腾讯会议最终
使用 DMA-BUF。本次测试同样没有产生 Wemeet core dump。

这些结果是有希望的开发阶段证据，但还不能视为长期稳定性保证。欢迎在严格
匹配版本的前提下测试并提交不包含私人数据的日志摘要或问题报告。
