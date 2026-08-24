# Wemeet PipeWire buffer guard (experimental)

An experimental lifetime guard for Tencent Meeting `3.26.10.401-5` on Hyprland/XDPH.
It contains no Tencent binaries.

## Problem addressed

After the Portal ordering bug is fixed, Tencent Meeting can retain a pointer to a
PipeWire SHM frame after the stream has released or removed that buffer. Observed
failures included:

- delayed `rep movsb` from an unmapped 14,745,600-byte SHM frame;
- `pw_stream_queue_buffer()+41` dereferencing a stale internal buffer reference;
- XDPH reporting `Out of buffers` while streams were destroyed and recreated.

The previous `straight` hook checked a mapping with `msync()` and then copied it.
That is a TOCTOU check: the mapping can disappear while the copy is running.

## Design

Tencent's screen-share module opens `libpipewire-0.3.so.0` itself and resolves the
API with `dlsym()`. The guard therefore interposes `dlsym()` and substitutes only
these functions:

- `pw_stream_new`
- `pw_stream_destroy`
- `pw_stream_add_listener`
- `pw_stream_connect`
- `pw_stream_disconnect`
- `pw_stream_dequeue_buffer`
- `pw_stream_queue_buffer`

For every dequeued frame, the guard copies the valid `spa_chunk` range while the
PipeWire buffer is still owned by the client. It temporarily replaces
`spa_data.data` with one of eight long-lived anonymous mirror mappings, then
restores PipeWire's original pointer before returning the buffer. The mirrors are
never unmapped before process exit, so delayed Tencent reads remain mapped.

Wrapped `add_buffer`, `remove_buffer`, `process`, disconnect and destroy events
track buffer lifetime. A completion callback that queues a removed or destroyed
buffer receives `-EPIPE`; the real `pw_stream_queue_buffer()` is not called.

Activation requires `WEMEET_PW_GUARD=1`; merely loading the library leaves all
`dlsym()` results untouched.

## Build and tests

Build products and test binaries intentionally go to `/tmp`:

```sh
make clean test
```

The mock PipeWire test verifies:

- snapshot and original-pointer restoration;
- mirror readability after the source mapping becomes inaccessible;
- rejection after `remove_buffer`, disconnect and destroy;
- 2,000 normal process/dequeue/queue cycles;
- a concurrent process/destroy race where destroy waits and the late queue is rejected;
- pass-through behavior when the guard is disabled.

Additional local checks passed with GCC `-fanalyzer`, ASan/UBSan, 50 repeated test
processes, and 300 full-size `2560×1440×4` frame cycles. ThreadSanitizer cannot
initialize with a custom `dlsym()` interposer even when this guard is disabled,
so the deterministic concurrent-destroy test is used for that path.

## Required companion changes

This library does **not** fix Portal call ordering or make DMA-BUF CPU-readable.
A controlled test additionally needs:

1. Hunk 0 only in `libscreen_share_module.so`;
2. XDPH `force_shm = 1`;
3. original, unpatched `libxcast.so`;
4. no `straight`/`swap` hook.

`tools/stage_hunk0.py` creates a validated staged copy and refuses in-place edits.
`tools/run-guarded.sh` overlays that copy only inside Wemeet's bubblewrap mount
namespace, so the host package remains byte-for-byte original. It also rebuilds
the `/tmp` runtime library automatically after a reboot. Start the experiment with:

```sh
~/Projects/wemeet-pw-guard/tools/run-guarded.sh
```

Do not use the normal `wemeet` launcher for a guarded test. Tencent's
`libwemeetwrap.so` must remain first in `LD_PRELOAD`; placing the guard first
breaks the wrapper's `RTLD_NEXT` lookups and leaves Wemeet without a visible UI.
The only host-side runtime setting needed is the removable XDPH SHM configuration.

The Hunk 0 offsets and bytes are from
[`GodKeawa/wemeet-bin-wayland-screenshare-fix`](https://github.com/GodKeawa/wemeet-bin-wayland-screenshare-fix),
not independently invented here.

## Current status

The guard passes synthetic tests and two real screen-sharing runs on either side
of a reboot. The second run delivered and encoded more than 700 `1920×1080`
frames and stopped cleanly, with no Wemeet core dump. The first run contained two
transient XDPH `Out of buffers` messages during initial renegotiation but recovered
and shared normally. Treat this as promising development evidence, not yet a
long-term stability guarantee.
