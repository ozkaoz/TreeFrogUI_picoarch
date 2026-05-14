#!/bin/bash
# Build picoarch LSB from f087301 (yesterday's working code)

docker run --rm -v "$PWD":/work -v /home/tomaszz/sf3000-work/sf3000toolchain:/tc -w /work bookworm_mips_docker bash -c '
echo "Building picoarch LSB from f087301 (yesterday working code)..."

TC="/tc/mipsel-buildroot-linux-gnu_sdk-buildroot/opt/ext-toolchain/bin/mips-mti-linux-gnu-"
SYSROOT="/tc/mipsel-buildroot-linux-gnu_sdk-buildroot/mipsel-buildroot-linux-gnu/sysroot"
SDL_CONFIG="/tc/mipsel-buildroot-linux-gnu_sdk-buildroot/mipsel-buildroot-linux-gnu/sysroot/usr/bin-o32/sdl-config"

CC_FLAGS="-mips32r2 -march=mips32r2 -mtune=24kc -mfp32 -mhard-float -mlong-calls -EL"
SDL_CFLAGS=$($SDL_CONFIG --cflags)

make clean
make CC="${TC}gcc" \
     CFLAGS="${CC_FLAGS} -Wall -fdata-sections -ffunction-sections -flto -I./ -I./libretro-common/include/ -I${SYSROOT}/usr/include ${SDL_CFLAGS}" \
     LDFLAGS="${CC_FLAGS} -lc -ldl -lgcc -lm -lSDL -lpng12 -lz -Wl,--gc-sections -flto" \
     -j4

if [ -f picoarch ]; then
    ${TC}strip picoarch
    echo ""
    echo "✓✓✓ LSB BUILD SUCCESS FROM YESTERDAYS WORKING CODE ✓✓✓"
    ls -la picoarch
    echo -n "Endianness: "
    head -1 picoarch | od -An -tx1 | head -1 | grep -q "01" && echo "LSB ✓✓✓" || echo "MSB ✗"
    du -h picoarch
    file picoarch
    echo ""
    echo "Deploying to SD card..."
    cp picoarch /run/media/tomaszz/596B-E19B/cubegm/picoarch
    echo "✓ Deployed to /run/media/tomaszz/596B-E19B/cubegm/picoarch"
else
    echo "✗ Build failed"
    exit 1
fi
'