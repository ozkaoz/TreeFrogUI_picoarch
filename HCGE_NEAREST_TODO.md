# TODO: HW nearest scaling via HCGE (SF3000)

Status: **parked**. Shipping uses SW nearest + HW bilinear (driver.so). This
documents the attempt to get HW *nearest* scaling and why it stalled, so it can
be resumed without re-deriving everything.

Experimental code lives on branch **`hcge-direct`** (`hcge_direct.c`,
`plat_sdl.c` gate, build-script entry). `main` is clean SW shipping.

---

## The goal

driver.so's `video_driver_disp_frame` HW-scales game frames to the panel but
**hardcodes bilinear** (interpolated). We want HW **nearest** (sharp integer
pixels) without the CPU cost of the current SW transpose path.

HCGE (the GE block driver.so wraps) supports both — `ge_api.h`:

```
HCGE_DSRO_NONE             = 0   /* nearest */
HCGE_DSRO_SMOOTH_UPSCALE   = 1   /* bilinear up */
HCGE_DSRO_SMOOTH_DOWNSCALE = 2
```

`render_options` field of `hcge_state` selects it. `HCGE_DSBLIT_ROTATE90`
handles the portrait-fb0 ↔ landscape-game rotation in the same blit.

---

## What was tried (branch `hcge-direct`)

`hcge_direct.c`: dlsym the exported low-level symbols from driver.so
(`hcge_open`, `hcge_state_init`, `hcge_set_state`, `hcge_stretch_blit`,
`hcge_engine_sync`), allocate an MMZ physical src buffer, point dst at fb0
`smem_start` (physical), build an `hcge_state` with `render_options=NONE` +
`ROTATE90`, and blit src→fb0.

Pipeline reached, per on-device trace:
- `hcge_open` OK (ctx allocated; ctx[344]=malloc(680) confirmed)
- `/dev/mmz` `MMZ_MEMALIGN` OK → physical src buffer, mmap OK
- fb0 dst phys = `0xaf92000`, 720x1280x32
- `hcge_direct_init: init ok`
- first `present`: logged `pre-set_state`, then **crash inside `hcge_set_state`**

## Root cause of the crash — struct ABI mismatch

`hcge_state_init` (driver.so @0xdbe8) `memset`s only **224 bytes** of the state
→ the driver's real `hcge_state` is ~224B.

But sysroot `ge_api.h`'s `hcge_state` is ~600+B (it inlines three full
`HCGE_CoreSurface` structs + `matrix[9]` + blend fields). **The header does not
match the driver.so ABI** (driver is an older HCGE; the sysroot header is newer).

Consequence: our writes (`src.phys`, `dst.phys`, `clip`, …) land at the wrong
byte offsets. `hcge_set_state` reads its real-ABI offsets, hits a garbage
pointer, segfaults. Cannot be fixed by tweaking field *values* — the layout is
wrong.

---

## Recommended approach (when resumed)

Do **not** keep guessing field offsets against `ge_api.h`. Two viable paths:

### Path A — patch driver.so's hardcoded smooth flag (preferred)

`hcge_state_init` defaults `render_options = 0` (NONE = nearest). So driver.so
**explicitly sets SMOOTH** somewhere before its blit. Find that and neutralize
it; then `video_driver_disp_frame` renders nearest with zero ABI risk (driver
uses its own correct struct).

Where to look:
- The blit happens in the **async render thread** `fb_render_task`, not in
  `video_driver_disp_frame` (which only stages frame params into ctx fields
  3484–3508 then signals the thread).
- Trace `fb_render_task` → its `hcge_set_state`/`hcge_stretch_blit` call →
  find the store of `1` (SMOOTH) into the state's `render_options` (offset 0 of
  the state it passes), or a ctx flag the thread ORs in.
- If unconditional: binary-patch the immediate `li …,1` → `li …,0`, or NOP the
  store. Ship the patched driver.so.
- If it reads a ctx/global flag: poke that flag to 0 from picoarch after
  `video_drivers_init` (no driver patch needed — cleanest).

Tooling: `mips-mti-linux-gnu-objdump -d driver.so`. Symbols of interest:
`fb_render_task`, `fbdev_draw_frame`, `hcge_set_state` (@0xca74),
`hcge_stretch_blit` (@0xa6b8), `designfilter`/`designfilterff` (interpolation
coeff calc — only called when smooth is on; a call-site to these = the smooth
gate).

### Path B — match the real hcge_state ABI

Reverse the true `hcge_state` layout from `hcge_state_init` (224B) +
`hcge_set_state` field reads, redefine the struct with correct offsets, retry
the direct path. Higher effort, fragile across driver versions. Only if Path A
proves the flag isn't reachable.

---

## Key facts / addresses (this driver.so)

| Item | Value |
|------|-------|
| fb0 phys (`smem_start`) | `0xaf92000` |
| fb0 geometry (SW) | 720×1280, 32bpp, pitch 2880 |
| MMZ alloc | `ioctl(/dev/mmz, MMZ_MEMALIGN={id:0,align:4096,size}, &blk)` → `blk.addr`=phys; mmap `/dev/mmz` at phys |
| MMZ_IOCBASE | 25; `MMZ_MEMALIGN=_IOWR(25,1,16B)` |
| DIS_IOCBASE | 14 (`/dev/dis` ioctl `0xc00c0e0c` = `_IOWR(14,0xc,12B)`) |
| `hcge_state_init` | @0xdbe8, memsets **224B** (= real state size) |
| `hcge_set_state` | @0xca74 (crash site) |
| `hcge_stretch_blit` | @0xa6b8 |
| `fbdev_draw_frame` | @0x5788 (stages ctx[3484..3508], signals render thread) |
| game src format | RGB565 (`HCGE_DSPF_RGB16`) |
| rotation | `HCGE_DSBLIT_ROTATE90` (portrait fb0 ← landscape game) |

## Test harness (branch)

- `PICOARCH_HCGE=1` env (or the forced `scale_filter==NEAREST && width<800` gate)
  routes game frames through `hcge_direct`.
- `/mnt/sdcard/hcge.log` — dedicated fsync'd HCGE step log.
- `/mnt/sdcard/picoarch_trace.log` — fsync'd main()/quit() milestones.
- Create `/mnt/sdcard/log.txt` to enable general `DBG`.
