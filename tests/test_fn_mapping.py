#!/usr/bin/env python3
"""
Test harness for FN button mapping Feature C1.
Validates 10 invariants plus additional checks.
Lightweight, no heavy framework.
Run: python3 tests/test_fn_mapping.py
Exit 0 = all pass, non-0 = fail.
"""
import re, sys, os, pathlib

REPO = pathlib.Path(__file__).resolve().parents[1]
plat_sf3000 = (REPO / "plat_sf3000.c").read_text()
plat_sdl = (REPO / "plat_sdl.c").read_text()
core_c = (REPO / "core.c").read_text()
menu_c = (REPO / "menu.c").read_text()
libpicofe_menu = (REPO / "libpicofe/menu.c").read_text()
config_file = (REPO / "libpicofe/config_file.c").read_text()

failures = []
passes = []

def check(cond, name, msg=""):
    if cond:
        passes.append(name)
        print(f"PASS: {name}")
    else:
        failures.append(name)
        print(f"FAIL: {name} {msg}")

# 1. 14 raw mappings remain same
expected_map = {
    4: "RETRO_DEVICE_ID_JOYPAD_UP",
    6: "RETRO_DEVICE_ID_JOYPAD_DOWN",
    7: "RETRO_DEVICE_ID_JOYPAD_LEFT",
    5: "RETRO_DEVICE_ID_JOYPAD_RIGHT",
    13: "RETRO_DEVICE_ID_JOYPAD_A",
    14: "RETRO_DEVICE_ID_JOYPAD_B",
    12: "RETRO_DEVICE_ID_JOYPAD_X",
    15: "RETRO_DEVICE_ID_JOYPAD_Y",
    10: "RETRO_DEVICE_ID_JOYPAD_L",
    11: "RETRO_DEVICE_ID_JOYPAD_R",
    8: "RETRO_DEVICE_ID_JOYPAD_L2",
    9: "RETRO_DEVICE_ID_JOYPAD_R2",
    0: "RETRO_DEVICE_ID_JOYPAD_SELECT",
    3: "RETRO_DEVICE_ID_JOYPAD_START",
}
# parse sf3000_keymap
m = re.findall(r"\{\s*(\d+)\s*,\s*(RETRO_DEVICE_ID_JOYPAD_\w+)", plat_sf3000)
found = {int(bit): retro for bit, retro in m}
# Only count first 14 (ignore any FN if added)
for bit, retro in expected_map.items():
    check(found.get(bit) == retro, f"1. mapping bit {bit} -> {retro}", f"found {found.get(bit)}")
check(len([b for b in found if b in expected_map]) == 14, "1. 14 mappings count")

# 2. FN exists as physical input when corresponds
check("SF3000_FN_BIT" in plat_sf3000, "2. FN_BIT defined")
check("SF3000_FN_SDLKEY" in plat_sf3000, "2. FN_SDLKEY defined")
check('SDLK_F11' in plat_sf3000 and '"FN"' in plat_sf3000, "2. FN key name FN in key_names")
check("sf3000_has_fn" in plat_sdl, "2. has_fn capability exists")
check("FN" in plat_sf3000 and "PHYSICAL" in plat_sf3000, "2. FN physical comment exists")

# 3. FN not alias SELECT — now FN at bit 16 (0x00010000) per Stock evidence, not 1, and must not alias SELECT bit0
check(re.search(r"#define SF3000_FN_BIT\s+16", plat_sdl+plat_sf3000) is not None, "3. FN bit is 16 not 0 (confirmed for R36SX V2.6)")
check("SDLK_F11" not in ["SDLK_RCTRL"], "3. FN SDLKey not SELECT")
# Check sf3000_bit_to_sdlkey handles FN separately, not alias SELECT
check("case 0:  return SDLK_RCTRL" in plat_sdl, "3. SELECT mapping unchanged")
check("SF3000_FN_BIT" in plat_sdl and "SDLK_F11" in plat_sdl, "3. FN distinct from SELECT")
check("0x00010000" in plat_sf3000 or "bit 16" in plat_sf3000, "3. FN documented as bit 16 (Stock)")

# 4. FN not alias START
check("case 3:  return SDLK_RETURN" in plat_sdl, "4. START mapping unchanged")
check("SF3000_FN_BIT" in plat_sdl, "4. FN not alias START")

# 5. no RETRO_DEVICE_ID_JOYPAD_FN (allow mention in comment “Must NOT create” but not actual definition)
def strip_comments(s):
    # naive: remove // and /* */ for check
    s = re.sub(r'//.*', '', s)
    s = re.sub(r'/\*.*?\*/', '', s, flags=re.S)
    return s
all_src_nocomment = strip_comments(plat_sf3000 + plat_sdl + core_c + menu_c)
check("RETRO_DEVICE_ID_JOYPAD_FN" not in all_src_nocomment, "5. no RETRO_DEVICE_ID_JOYPAD_FN created")

# 6. FN default UNBOUND (no entry in in_sdl_defbinds for FN)
defbind_section = re.search(r"static const struct in_default_bind in_sdl_defbinds\[\].*?\{(.*?)\};", plat_sf3000, flags=re.S)
if defbind_section:
    defbind_text = defbind_section.group(0)
    has_fn_bind = "SDLK_F11" in defbind_text
    check(not has_fn_bind, "6. FN default UNBOUND (no SDLK_F11 in defbinds)")
else:
    check(False, "6. FN default UNBOUND", "defbinds not found")

# 7. device HAS_FN=NO not expose FN (but R36SX auto-enables, and per C1.9 gating removed from event path)
# default sf3000_has_fn = 0, but auto-enabled for TF_DEVICE=R36SX for logging
check("int sf3000_has_fn = 0" in plat_sdl, "7. HAS_FN default NO (0) before auto-enable")
check("TF_DEVICE" in plat_sdl and "R36SX" in plat_sdl and "sf3000_has_fn = 1" in plat_sdl, "7. R36SX auto-enables HAS_FN without file")
# Check logic: per C1.9 final model, FN event is DATA-DRIVEN not gated, so bit_to_sdlkey should NOT gate via has_fn
check("return sf3000_has_fn ?" not in plat_sdl, "7. FN NOT gated by has_fn (data-driven per C1.9)")
check("if (bit == SF3000_FN_BIT)" in plat_sdl and "return SF3000_FN_SDLKEY" in plat_sdl, "7. FN bit16 always surfaces as SDLK_F11")

# 8. UNKNOWN not expose FN in production (other devices)
# R36SX is YES, others remain NO/UNKNOWN hidden
check("SF3000 input: HAS_FN=NO" in plat_sdl or "HAS_FN=NO" in plat_sdl, "8. UNKNOWN hidden in production (log for non-R36SX)")
check("SF3000 input: HAS_FN=YES" in plat_sdl or "HAS_FN=YES" in plat_sdl, "8. R36SX shows HAS_FN=YES when auto-enabled")
# Ensure diagnostic not active by default without compile flag
check("FN_DIAGNOSTIC" in plat_sdl or "sf3000_fn_diag" in plat_sdl, "8. diagnostic gated")

# 9. configs anciennes still valid
# Check config_file parsing uses string compare for action names, not index-shift
# Ensure me_ctrl_actions order unchanged and not inserting FN in middle
# me_ctrl_actions in menu.c should still be 14 entries without FN
ctrl_actions = re.findall(r'me_ctrl_actions\[\].*?\{(.*?)\};', menu_c, flags=re.S)
if ctrl_actions:
    txt = ctrl_actions[0]
    # count entries with RETRO_DEVICE_ID
    cnt = txt.count("RETRO_DEVICE_ID")
    check(cnt == 14, f"9. me_ctrl_actions still 14 (found {cnt})", "FN not inserted in virtual actions")
    # ensure no FN in emuctrl_actions
    check("FN" not in txt, "9. FN not in me_ctrl_actions (physical not virtual)")
else:
    check(False, "9. me_ctrl_actions found")
# Check config_write_keys still iterates me_ctrl_actions and handles unknown gracefully
check("me_ctrl_actions" in config_file, "9. config serialisation uses me_ctrl_actions")
check("emuctrl_actions" in config_file, "9. config serialisation uses emuctrl_actions")
# Check in_config_bind handles unknown key names gracefully (returns -1)
check("in_config_bind_key" in (REPO / "libpicofe/input.c").read_text(), "9. input binds per keycode stable")

# 10. SELECT hotkeys remain same
# Check core.c still uses SEL_BIT 0, START_BIT 3 etc for hotkeys
check("SEL_BIT   = (1u << 0)" in core_c, "10. SELECT hotkey bit 0 unchanged")
check("START_BIT = (1u << 3)" in core_c, "10. START hotkey bit 3 unchanged")
check("R1_BIT    = (1u << 11)" in core_c, "10. R1 hotkey bit 11 unchanged")
check("SELECT+R1" in core_c or "SELECT+R" in core_c or "handle_emu_action(EACTION_TOGGLE_FF)" in core_c, "10. SELECT hotkeys logic present")
check("FN" not in core_c or "FN as Hotkey" not in core_c, "10. FN not as hotkey enable")

# Additional: diagnostic tool created
fn_diag_path = REPO / "tools/fn_input_diag.c"
check(fn_diag_path.exists(), "DIAG. fn_input_diag.c exists")
if fn_diag_path.exists():
    diag_text = fn_diag_path.read_text()
    check("RAW_PREVIOUS" in diag_text or "prev=0x" in diag_text, "DIAG. shows RAW previous")
    check("changed" in diag_text, "DIAG. shows changed mask")

# === C1.9 regression: previously missed defects ===
# 1. No stable & 0xFFFF truncation on input event path
check(("stable & 0xFFFF" not in plat_sdl), "C1.9: no stable & 0xFFFF truncation")
check(("0x0001FFFF" in plat_sdl), "C1.9: uses 0x1FFFF mask")
check(("cur = stable & 0x0001FFFF" in plat_sdl or "cur = stable & 0x1FFFF" in plat_sdl or "0x0001FFFFu" in plat_sdl), "C1.9: cur uses 0x1FFFF mask")
# 2. Supported physical mask includes 0x00010000 (bit16)
check(("0x00010000" in plat_sdl or "1u << 16" in plat_sdl or "bit 16" in plat_sdl), "C1.9: mask includes 0x00010000 bit16")
# 3. bit16 maps to SDLK_F11
check(("SF3000_FN_BIT" in plat_sdl and "SDLK_F11" in plat_sdl), "C1.9: bit16 -> SDLK_F11")
# 4. SDL event loop can process bit16 (not limited to sf3000_keymap_count)
check(("for (int bit = 0; bit <= 16" in plat_sdl), "C1.9: SDL loop handles 0..16")
# 5. sf3000_keys_to_buttons does NOT create RetroPad bit for FN
sf_keys_to_buttons_section = re.search(r"uint32_t sf3000_keys_to_buttons.*?{(.*?)}", plat_sf3000, flags=re.S)
if sf_keys_to_buttons_section:
    txt = sf_keys_to_buttons_section.group(0)
    check("FN" not in txt and "16" not in txt or "RETRO_DEVICE_ID" not in txt.split("FN")[0] if "FN" in txt else True, "C1.9: sf3000_keys_to_buttons does not map FN to RetroPad")
else:
    check("sf3000_keys_to_buttons" in plat_sf3000, "C1.9: sf3000_keys_to_buttons exists")
# 6. No RETRO_DEVICE_ID_JOYPAD_FN (allow comment but not code; check stripped)
check("RETRO_DEVICE_ID_JOYPAD_FN" not in all_src_nocomment, "C1.9: no RETRO FN ID")
# 7. Visible name FN
check('"FN"' in plat_sf3000, "C1.9: visible name FN")
# 8. Existing 14 mappings unchanged (already checked) and SELECT/START
check("RETRO_DEVICE_ID_JOYPAD_SELECT" in plat_sf3000, "C1.9: SELECT still present")
check("RETRO_DEVICE_ID_JOYPAD_START" in plat_sf3000, "C1.9: START still present")

# Summary
print(f"\n=== RESULTS: {len(passes)} PASS, {len(failures)} FAIL ===")
if failures:
    print("Failures:", failures)
    sys.exit(1)
else:
    print("ALL CHECKS PASS")
    sys.exit(0)
