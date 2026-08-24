# AGENTS.md — TreeFrogUI_picoarch — TreeFrogUI R36SX Fork Component

**Repo:** https://github.com/ozkaoz/TreeFrogUI_picoarch (fork), upstream `https://github.com/tzubertowski/TreeFrogUI_picoarch`
**Branch:** `r36sx`
**Protocol:** `TREEFROGUI_AGENT_PROTOCOL=1`
**Parent coordination:** `../../treefrog-ui-r36sx/docs/ai/MULTIREPO_COORDINATION.md`

> Local rules for the **picoarch** fork (libretro host, display/audio, input translation). Parent `treefrog-ui-r36sx` integrates `picoarch`/`picoarch_hi` binaries via `build_release.sh`.

## 1. Purpose

picoarch is the libretro frontend for Hichip MIPS handhelds (R36SX/SF3000 family). This fork (`r36sx` branch) provides TreeFrogUI integration: display (`fbwrite` R36SX 640×480 vs `dispframe` SF 854×480), audio ALSA, input `cubevol → /tmp/joy_key` → RetroPad, core execution (`picoarch` normal vs `picoarch_hi` for `gpsp`/`pcsx` high dynarec), and `zhijack.sh` device hardcoding.

## 2. Canonical env

- **WSL Ubuntu** (`~/sf3000-work/TreeFrogUI_picoarch`, or `/mnt/d/R36SX/TreeFrogUI_picoarch` mirror). Do not build in PowerShell.
- Toolchain `~/sf3000-work/sf3000toolchain/mipsel-buildroot-linux-gnu_sdk-buildroot` (`mips-mti-linux-gnu-gcc 6.3.0`), same as parent.
- Parent workspace: `~/sf3000-work/{treefrog-ui-r36sx,FrogUI,TreeFrogUI_picoarch,cores,sf3000toolchain}`.

## 3. Branch discipline

- `r36sx` — TreeFrogUI integration. Base for `r36sx-v2.6-dev`.
- Feature branches: `feature/r36sx-<topic>` from `r36sx` (e.g., `feature/fn-button-mapping`).
- No `git push upstream` without auth; push to `origin` `ozkaoz/TreeFrogUI_picoarch` only. No `force push` after review.

## 4. Build

```sh
# WSL, in this repo (~/sf3000-work/TreeFrogUI_picoarch)
sh build_sf3000.sh          # → picoarch (normal)
sh build_picoarch_hi.sh     # → picoarch_hi (high, for gpsp/pcsx)
# or
make -f Makefile.sf3000  # if present
ls -lh picoarch picoarch_hi
file picoarch  # ELF 32-bit LSB MIPS32r2
```

Artifacts: `picoarch`, `picoarch_hi` → parent `treefrog-ui-r36sx/sdcard/cubegm/` → `release/latest/release/cubegm/` via `build_release.sh:35-36` (`PICOARCH`/`PICOARCH_HI` -> `cp_if_diff`).

Flags: `-mips32r2 -march=mips32r2 -mtune=74kc -mdspr2 -mhard-float -EL` (parent `build_all.sh` `SF3000_FLAGS`, `docs/BUILDING.md`).

## 5. Platform invariants (derive from source, do not invent)

- **Input translation:** `cubevol` daemon `gpio → /tmp/joy_key` shm, read by picoarch, translated to RetroPad. **Do not** invent libretro button IDs or change `RETRO_DEVICE_ID_JOYPAD_*` mapping without source evidence (`picoarch/input.c`, `frogui/input.c`, `docs/HARDWARE.md` FN 16/L3 1/R3 2).
  - Preserve unrelated hotkeys: `SELECT+START` menu, `SELECT+R2/L2` save/load, `SELECT+R1` fast-forward, `SELECT+B` rewind, `SELECT+L1` screenshot (see parent `README.md#Shortcuts`).
  - R36SX right analog mirrors face buttons (hardware, not analog) — picoarch must not claim analog values.
- **Display:** `TF_DEVICE`, `TF_PANEL_W/H`, `TF_ASPECT_NUM/DEN`, `TF_ROTATE`, `TF_PRESENT` (`fbwrite` R36SX vs `dispframe` SF) via `/tmp/tfdevice.env` written by `hijack/zhijack.tpl.sh` (parent). Picoarch reads this, no DT probing.
- **Audio:** ALSA via `TF_DRIVER` (`driver_r36sx.so` vs `driver_r36sx27.so` SIGBUS fallback, `driver_sf3500.so` etc.), `LD_LIBRARY_PATH` `cubegm/lib`. No external invariants.
- **Normal vs HI:** `picoarch_hi` is high dynarec build of same source for `gpsp`/`pcsx` (parent `build_release.sh:WARN picoarch is newer than picoarch_hi`). Keep `picoarch`/`picoarch_hi` in sync — `picoarch_hi` must not be older than `picoarch`.
- **Boot:** `hijack/zhijack.tpl.sh` `kill -STOP icube` + `killall rkgame` (R36SX `stop` 1×, SF `kill` 3×) — picoarch assumes `cubevol` is up after `zhijack` (`sleep 0.5`, `pidof cubevol`).

## 6. Tests

- Host: `sh build_picoarch_host.sh` (if present) for Linux host testing.
- Parent contract: `python tests/test_agent_context_contract.py` (parent) + `sh hijack/build_tfhijack.sh` integration.
- For input/audio changes: parent `PACKAGING PASS` + `PHYSICAL` on R36SX V2.6 (`docs/TESTING.md` parent, `docs/ai/VALIDATION.md` CLASS C/D `STATIC+BUILD+HOST+PHYSICAL`).

## 7. Physical validation

- `BUILD PASS` ≠ `PHYSICAL PASS`. Agents never claim `PHYSICAL PASS` without human hardware report (`FIRMWARE/BASELINE`, `ARTIFACT_SHA256`, `TEST_MATRIX`, `USER_OBSERVATIONS`, `PASS/FAIL`, `DATE`).
- Cross-repo (FrogUI+picoarch) → parent integration artifact + `PHYSICAL` matrix (`docs/ai/MULTIREPO_COORDINATION.md` §7).

## 8. Git safety

- `git commit`/`push`/`tag` = `ask`. No `reset --hard`, `clean -fd`, `restore` destructive, `rm -rf`, `force push`.
- Focused commits, no unrelated changes. Each repo separate history — no parent commit for child-only change.
- Verify `git status --porcelain`, `git rev-parse HEAD`, `git remote -v` before edit.

## 9. Cross-repo handoff

After change, report to parent:

```
REPOSITORY=TreeFrogUI_picoarch BRANCH=feature/r36sx-foo BASE=
ARTIFACTS=picoarch (→) picoarch_hi SHA256=... BUILD_RESULT=PASS
RELATED_PRS= https://github.com/tzubertowski/TreeFrogUI_picoarch/pull/...
NEXT_EXACT_ACTION= Parent assembles release/latest/release and human tests R36SX
```

See `docs/ai/MULTIREPO_COORDINATION.md` §4.

## 10. Protocol

```
TREEFROGUI_AGENT_PROTOCOL=1
```
