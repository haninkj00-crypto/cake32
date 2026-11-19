set -eu

CC=${CC:-i686-w64-mingw32-gcc}
WINDRES=${WINDRES:-i686-w64-mingw32-windres}

"$WINDRES" cake32.rc -O coff -o cake32_res.o

"$CC" -std=c89 -Os -Wall -Wextra \
  -ffreestanding -fno-builtin -fno-stack-protector \
  -fno-asynchronous-unwind-tables -fno-unwind-tables -mfpmath=387 \
  -c cake32.c -o cake32.o

"$CC" -nostdlib \
  -Wl,--subsystem,windows \
  -Wl,--major-subsystem-version,3 \
  -Wl,--minor-subsystem-version,10 \
  -Wl,--image-base,0x00400000 \
  -Wl,--disable-dynamicbase \
  -Wl,--disable-nxcompat \
  -Wl,--entry,_CakeEntry \
  cake32.o cake32_res.o -lkernel32 -luser32 -lgdi32 \
  -o CAKE32.exe

printf '%s\n' 'Built CAKE32.exe'
