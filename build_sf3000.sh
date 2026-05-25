#!/bin/bash
# Build picoarch for SF3000 using Docker + cross-toolchain
# Only builds the picoarch binary (not cores — those need separate builds)
# Target: MIPS32r2 little-endian (LSB) — matching SF3000 hardware

set -e

TC="/tc/mipsel-buildroot-linux-gnu_sdk-buildroot/opt/ext-toolchain/bin/mips-mti-linux-gnu-"
SYSROOT="/tc/mipsel-buildroot-linux-gnu_sdk-buildroot/mipsel-buildroot-linux-gnu/sysroot"
SDL_CONFIG="${SYSROOT}/usr/bin-o32/sdl-config"

CC_FLAGS="-mips32r2 -march=mips32r2 -mtune=24kc -mfp32 -mhard-float -mlong-calls -EL"
SDL_CFLAGS=$($SDL_CONFIG --cflags)

echo "=== Building picoarch for SF3000 (LSB / little-endian) ==="
echo "CC: ${TC}gcc"
echo "SYSROOT: ${SYSROOT}"
echo "SDL_CFLAGS: ${SDL_CFLAGS}"

# Clean only picoarch objects (not cores)
rm -f libpicofe/input.o libpicofe/in_sdl.o libpicofe/linux/in_evdev.o \
      libpicofe/linux/plat.o libpicofe/fonts.o libpicofe/readpng.o \
      libpicofe/config_file.o cheat.o config.o content.o core.o menu.o \
      main.o options.o overrides.o patch.o scale.o scaler_neon.o \
      unzip.o util.o plat_sf3000.o hwdisp.o picoarch

# Build only the picoarch binary (not cores)
# --sysroot tells the linker where to find -lSDL, -lpng12, -lz etc.
make platform=sf3000 \
     CC="${TC}gcc" \
     SYSROOT="${SYSROOT}" \
     CFLAGS="${CC_FLAGS} --sysroot=${SYSROOT} -Wall -fdata-sections -ffunction-sections -I./ -I./libretro-common/include/ -I${SYSROOT}/usr/include ${SDL_CFLAGS} -DPICO_HOME_DIR='\"/.picoarch/\"' -DCONTENT_DIR='\"/mnt/SDCARD/Roms\"' -DUSE_C_SCALER -DPLATFORM_SF3000 -Ofast -DNDEBUG" \
     LDFLAGS="${CC_FLAGS} --sysroot=${SYSROOT} -L${SYSROOT}/usr/lib -lc -ldl -lgcc -lm -lSDL -lpng12 -lz -Wl,--gc-sections -lpthread -s" \
     picoarch \
     -j4

if [ -f picoarch ]; then
    ${TC}strip picoarch
    echo ""
    echo "=== BUILD SUCCESS ==="
    ls -la picoarch
    file picoarch
    # Verify little-endian
    FILE_INFO=$(file picoarch)
    if echo "$FILE_INFO" | grep -qi "LSB\|little.endian"; then
        echo "✓ Endianness: LSB (little-endian) — correct for SF3000"
    else
        echo "✗ WARNING: Binary may not be little-endian!"
        echo "  $FILE_INFO"
    fi
else
    echo "=== BUILD FAILED ==="
    exit 1
fi
