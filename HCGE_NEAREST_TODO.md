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

---

## 2026-06 session: driver.so live-patch attempt — FAILED, here's why

Tried the simplest Path-A idea (flip the smooth flag in the loaded `driver.so`,
per-filter, before the render thread spawns). **Got the patch fully working and
verified on device — but it had ZERO visual effect.** Both nearest and bilinear
render identical (bilinear). Confirmed via on-device log (`init: … want=1
pre-patch=0 now=240a0000`, gambatte `present#1 w=160 h=144 instr=240a0000`).

What was found in *this* driver.so (device copy, md5 `83ceee0d…`, addresses match
the table above — NOT mini_rkgame's `dfc0…`):

- **`render_options` = byte at hcge_state +269 (0x10D).** `hcge_stretch_blit`
  @0xa6b8 reads it (`lbu v0,269; beqz → skip designfilter`).
- **`hcge_set_state` @0xca74 sets +269 = bit2 of source-state word at +200
  (0xC8)** (`lw v0,200; ext v0,v0,2,1; sb v0,269`).
- **`fb_paint_task` @0x4e98 writes +200 = 4 unconditionally** (`li t2,4` @0x51a8,
  fileoff==vaddr; `04000a24`). So render_options is always 1 = SMOOTH_UPSCALE.
- Patched `li t2,4` → `li t2,0` (state[200]=0 → render_options=NONE) via picoarch
  live mprotect+`__builtin___clear_cache`, applied **before** `video_drivers_init`
  spawns the render thread (cold icache). **No effect.**
- Also tried clearing the stretch filter-field bits in `hcge_set_state`
  (`v0[100]` bits 25/26/27 @0xcc80/cc88/cc90, `ins …,a0` → `ins …,zero`). **No
  effect, no glitch.**

**Root cause / conclusion:** per `ge_api.h`, `HCGE_DSRO_NONE=0` means "no
interpolation for upscale" — but this driver implements NONE as **skip
designfilter** (skip coefficient *reload*), NOT "load a nearest/delta kernel".
The HCGE polyphase scaler (`designfilter → tunefilter → extract_coef/extract_phase`,
@0xe4dc/0xe3b0/0x10528/0x104e8) keeps its previously-loaded/default (smooth)
coefficients, so it still interpolates. **There is no nearest code path in the
driver.** True HW nearest would require *forging a delta coefficient kernel*
into the scaler (reverse `extract_coef`/`extract_phase` math + inject) — deep,
fragile, not attempted.

`video_driver_setting`/`setmode` only stash scale dims into globals
`G[0x3290/0x3294/0x3298]`; no filter selector. `fbdev_set_enhance` @0x6ff0 =
sharpness/contrast ioctls (0x40180d02 / 0x80180d01), not the scaler filter.

### Attempt 2 (also parked): pretty HW-bilinear FrogUI + SW nearest games

Goal: FrogUI menu always HW bilinear (looks great), games selectable
bilinear=HW / nearest=pure-SW (sharp+fast). Tried in `plat_sdl.c sf3000_fb_blit`:
- FrogUI 854×480 frames → `hwdisp_init` on demand + `hwdisp_present` (HW bilinear,
  1:1 rotate). **This worked — FrogUI was pretty.**
- nearest games → skip HW (warm-boot early-init in `sf3000_fb_init` gated off when
  `frogui/settings.txt filter=nearest`), use pure-SW path.

**Blocker — HW→SW display-controller state leak:** a nearest game launched *from*
the (HW) FrogUI comes up **squished to the right** (colors fine = it's a
geometry/scaler issue, not pixel format). `sf3000_fb_init` FBIOPUTs fb0 back to
720×1280 32bpp + FBIOPAN(0,0) + blank-cycle, but that only resets the **fb
geometry** — the **display-controller scaler** (the `scale.h_div/v_div/h_mul/v_mul`
+ rotate config the driver programs via `/dev/dis`, seen in logs as
`video_driver_setting 854 480 …`) stays in FrogUI's HW config, so the SW frame
gets re-scaled/panned → squish. Tried: skipping HW early-init for nearest (not
enough — the *previous* FrogUI process already left the scaler programmed);
deinit-gate in-process (worse — strands fb0 in HW mode → row tearing).

**Next time:** to mix HW-FrogUI with SW-games, the SW path (or `sf3000_fb_init`)
must fully **reset the display-controller scaler to identity/passthrough** on a
HW→SW transition — find the `/dev/dis` ioctl (or `video_driver_setmode`/`setting`
sequence) that sets `scale.*`+rotate and program it for 1:1 720×1280, not just
FBIOPUT. OR keep FrogUI on SW too (no mixing) and only chase real HW nearest
(coefficient forging, Attempt 1). `present_integer` (sharp HW) is ~40fps — too
slow vs pure SW which is sharp AND full-speed for small cores.

### What shipped instead (this session)

`hwdisp_present_integer` (already in hwdisp.c): SW nearest integer-replicate the
small game frame (e.g. GB 160×144 → 480×432, 3×) into an **854×480 panel buffer**,
hand to `video_driver_disp_frame` at panel size → driver does a pure **1:1
rotate** (no magnification → no interpolation → sharp) + HW DMA present. Wired in
`plat_sdl.c sf3000_fb_blit`: `integer_mode = (scale_filter==NEAREST && width<800)`
→ `hwdisp_present_integer`, else `hwdisp_present` (driver bilinear fill). Only the
cheap pixel replicate is SW; rotate+present are HW. This is the practical sharp
result; pure zero-SW HW nearest is parked pending the coefficient-forging work.
