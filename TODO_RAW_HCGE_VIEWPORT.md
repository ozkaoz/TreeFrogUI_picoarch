# TODO: raw HCGE viewport for SF3000/SF3500

The SF3000/SF3500 stock display driver accepts the source frame and a
fit/fill flag, but does not expose an arbitrary destination viewport or crop.
Because the chipset is extremely slow, do not implement this by reshaping or
scaling a frame in software.

Investigate the raw HCGE/display-controller API for a hardware destination
rectangle (including the 270-degree rotation path), then restore custom 4:3,
16:9, 3:2, 5:4, 8:7, and 16:10 modes on SF3000/SF3500 once the viewport is
programmed directly.

## Findings (2026-08-27)

* SF3000 is using the proprietary `driver_sf3000.so`/HCGE path. The deployed
  binary hashes matched the local build; this is not a stale-binary problem.
* The stock driver reports a portrait source setup (`640x480`, scale tuple
  `640,480 -> 480,854`, rotation `270`) and repeatedly calls
  `video_driver_setting` for NES frames (`256x224`, pitch `512`). The panel
  framebuffer is temporarily changed by the driver to `1280x720`/16bpp and
  restored to `720x1280`/32bpp during process transitions.
* Reverse engineering of `video_driver_disp_frame` shows hard-coded `640` and
  `480` constants in its setup path. `video_driver_setting` copies only the
  colour format, image width and image height/pitch words into the live HCGE
  context; the reported screen-width/screen-height words are ignored.
* The live context contains scale fields around offsets `0xda8..0xdb8` and
  canvas fields around `0x3290..0x3298`. These are likely inputs for a future
  hardware viewport implementation. Directly NOP-ing the stores at
  `disp_frame+0xa8/+0xb0` caused black screens because those stores initialise
  required HCGE state; do not deploy that patch.
* The kernel HCFB ioctl probe (`GET_SCALE_ONOFF`, `SET_SCALE`) returns success
  on the target, but only the stock `640,480 -> 480,854` tuple has been tested.
  No arbitrary aspect ratio has yet been proven through this interface.
* SF3000 must remain hardware-only for normal presentation. The old software
  full-panel scaling path caused severe slowdown/audio stutter and must not be
  reintroduced as a “fallback”.
* A first diagnostic logger placed after the display path produced no game
  records, even though Mario launch history was present. The logger placement,
  not deployment, was wrong. The current diagnostic build logs at the first
  `pa_video_refresh()` entry to the established `log.txt` channel, before the
  `data`/shutdown guard, with PID, pointer, dimensions, pitch, and quit state.
  This is the next required test to establish whether the core callback is
  delivering frames and where the SF3000 path diverges.
* The follow-up test deployed the exact diagnostic hashes and launched the NES
  core. The log shows `Loading core ... fceumm`, `Screen: 256x224`, and then
  shutdown, but no `video_refresh entry` for the game (only the preceding
  FrogUI `854x480` callback). Therefore this particular run exited before the
  first libretro frame; its later `video_driver_setting 256x224` messages are
  driver initialisation, not proof that a game frame reached the scaler. Fix or
  instrument the launch/retro_run lifecycle before drawing further aspect-ratio
  conclusions from that run.
* User testing confirms the game itself renders and plays normally. On SF3000,
  every mode except `Fill` currently looks effectively identical; `Fill` is the
  only mode that visibly changes the result. This points to the selected aspect
  mode not reaching the driver's destination geometry (or the driver collapsing
  all fit modes to its stock `640x480 -> 480x854` transform), rather than a
  core-launch or performance failure.
* The HCFB ioctl was tested on-device: it returns `0`, but changing the
  proposed output width visibly squishes both FrogUI and games. It is disabled
  at runtime and retained only as a probe.
* The exported `video_driver_setmode(0, mode)` API was tested. The stock binary
  reports mode `1` as 4:3, but switching modes does not change the rendered
  viewport on SF3000; it is not an arbitrary-ratio control.
* Writing live render-context canvas fields `ctx+0x3290/+0x3294` after
  `disp_frame()` also has no visible effect. `fb_paint_task` copies the context
  into a private HCGE state and computes the destination rectangle locally.
* Current disassembly target: `fb_paint_task` writes the actual destination
  rectangle into private state offsets `+0xb0/+0xb4/+0xb8/+0xbc` in normal and
  rotation branches immediately before the stretch-blit call. The next hook
  must modify those state values or intercept `hcge_stretch_blit`; NOP-ing the
  initialization stores is unsafe and previously caused black screens.
