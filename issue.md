# SF3000 HW path: cold-boot constraint + warmboot recovery

## Hardware context

- SF3000: MIPS32r2 handheld. Physical display 854×480 landscape.
- `/dev/fb0`: 720×1280 portrait 32bpp ARGB (kernel default). Display controller rotates 90° to landscape.
- `/dev/fb1`: HCFB overlay layer (cubevol battery/volume OSD).
- `driver.so` at `/mnt/sdcard/cubegm/driver.so` — proprietary HCGE compositor. Exports:
  `video_drivers_init()`, `video_driver_deinit()`, `video_driver_disp_frame(src, w, h, pitch)`.
  When active: reconfigures fb0 to 1280×720×16bpp and routes display through its own HW pipeline.
- **SF3000 presentation**: hardware-only via `driver.so`/HCGE. The legacy
  mmap/transpose framebuffer path is no longer used on SF3000.
- **R36SX presentation**: retains its separate direct framebuffer path.
- **HW path**: driver.so via `hwdisp_init()`. Bilinear = pass src directly to `p_disp`, driver HW-scales to panel.
  2-3 frame input lag. Smooth.

## The core constraint (verified empirically)

**`driver.so`'s FIRST `p_disp` call ever (on a cold kernel session) hangs indefinitely.**

Confirmed with `/mnt/sdcard/picoarch_present.log` showing `#1 enter ... #1 pre-p_disp` then nothing.
Tried and ruled out as the cause:
- input size (160×144, 320×180, 853×480, 854×480 all hang)
- `/dev/fb1` touch / `fb1_blank` before init
- `/dev/mmz`, `/dev/ge`, `/dev/dis` open primes
- `FBIOBLANK(1)` + `FBIOBLANK(0)` cycle
- 8s sleep before `video_drivers_init`
- `hwdisp_init` + `hwdisp_deinit` + `hwdisp_init` cycle in same process
- `fork()` prime in child process
- 854×480 → 320×180 downscale before `p_disp`

Why games work: games are *never* the first process. icube launches FrogUI first.
Then FrogUI execs to the game process via `quit()` — **same PID**, kernel-side state primed by
FrogUI's prior fb0 activity.

## Solution: warmboot marker

When a picoarch process exits with `sf3000_use_hwdisp` still set (HW game exiting), it leaves
a marker at `/tmp/picoarch_hcge_was_active` containing its `getppid()` (= icube shell child PID).

The next picoarch process (executed via `execl` from `quit()` — same PID, same PPID) reads the
marker in `sf3000_fb_init`. If PPID matches (same icube session, not a stale marker from a
prior reboot), it calls `hwdisp_init()` *early* — before any `retro_run` frames.
`sf3000_use_hwdisp` is set to 1.

Then in `sf3000_fb_blit`, FrogUI's panel-native 854×480 frames are routed straight to
`hwdisp_present` with `filter=0` (pass-through, no aspect padding). The display controller
stays in HW state; no SW→HW or HW→SW transition needed.

Visual cost: one or two frames of flicker as panel re-syncs during the exec transition.
Functional: FrogUI renders correctly, no squish.

## Why this works

- Game's `quit()` → marker written → `execl` FrogUI → marker read → `hwdisp_init` succeeds
  because the kernel session has already had successful `p_disp` calls (during the game).
- Cold boot FrogUI → hardware driver is initialized before the first frame.
- Cold boot game (impossible — icube always launches FrogUI first).

## Files involved

| File | What |
|------|------|
| `plat_sdl.c` `sf3000_fb_init` | Marker read + early `hwdisp_init` |
| `plat_sdl.c` `sf3000_fb_blit` | FrogUI 854×480 + warm `sf3000_use_hwdisp` → HW pass-through |
| `main.c` `quit()` | Marker write when `sf3000_use_hwdisp` set |

## Hard constraints (do not break)

- SF3000 must remain hardware-only; if `hwdisp_init()` fails, startup fails
  instead of falling back to software framebuffer writes.
- Marker check must verify `getppid()` to avoid acting on stale marker from prior reboot
  (`/tmp` may persist on some kernels).
- FrogUI's HW path: must use `filter=0`, no aspect padding. `filter=1` (nearest SW-upscale)
  on 854×480 input hard-crashes the device.

## Filter configuration (FrogUI Settings)

The nearest/bilinear filter is configured in FrogUI's Settings menu (saved to
`/mnt/sdcard/frogui/settings.txt`). Picoarch reads this on startup via `load_frogui_filter()`
in `main()`. FrogUI itself ignores the setting (always SW); games respect it.

Filter changes do not require an in-game restart — apply on next game launch.
