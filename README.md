# cake32
I've ported the Cake.exe by Charles Petzold.

This is not the remaster. I tried to preserve as much original logics as possible.

However, I prioritized the original *behaviors* than *source codes*, since the purpose is to run on 64bit(which doesn't mean it is a 64-bit program.)

Therefore, no license, no GNU, no MIT. I cannot grant you any licenses because this is Charles Petzold's work, not mine.

However, I hope you review my code, ask to LLMs, etc.






# CAKE32 — Win32/x86 port of Charles Petzold's CAKE (November 1985)

This is a 32-bit x86 Win32 rewrite of Petzold's Win16 sample. It is intended
for Windows 11 x64 through WOW64, while keeping the executable format and API
imports conservative for a possible Windows 3.1 + Win32s run.

It is a PE32 application, not a 16-bit Windows application. It cannot run
natively on Windows 1.x/2.x; Win32s requires Windows 3.1. Win32s compatibility
has not been runtime-tested in this build environment.

## Files

- `cake32.c` — application, drawing, animation timing, and ANSI text handling.
- `cake32.h` — resource identifiers.
- `cake32.rc` — English-only menu and dialog resources.
- `build-msvc-x86.cmd` — MSVC x86 build.
- `build-mingw-x86.sh` — MinGW-w64 i686 build.

## Candle count

There is no 0–50 restriction. The settings dialog accepts **0 through
4,294,967,294**. The sole upper bound is the largest 32-bit unsigned
count for which `count + 1` still fits in a 32-bit denominator; it is not a
visual or arbitrary performance cap.

The drawing algorithm does not loop once for every requested candle. Candle
positions are confined to the 560-pixel horizontal interval from x=40 to x=599.
When multiple candles land on the same x coordinate, candles with the same
vertical offset completely overwrite earlier candles of that offset. The port
therefore draws only the last candle for each x coordinate and vertical offset.
This preserves the final candle geometry while bounding drawing work to at
most 1,120 candles, even when a very large count is entered. Flame drawing uses
the same positional reduction. The exact pseudo-random flame sequence can
therefore differ from a literal per-candle loop at very high counts.

## Timing model

The timer resolution used by classic IBM PC-compatible Windows is based on the
PIT input clock:

\[
\frac{1,193,182}{65,536}=18.20651245\text{ ticks/s}
\]

One tick is approximately **54.925401 ms**.

The original source calls:

- `SetTimer(hwnd, 1, 1, NULL)` for candle animation;
- `SetTimer(hwnd, 3, 1, NULL)` for fireworks;
- `SetTimer(hwnd, 2, 250, NULL)` for text flashing.

Petzold's Windows timer discussion explains that the interval is rounded down
to an integral number of these ticks. Intervals shorter than one tick therefore
produce at most one timer event per tick. The original 250 ms request becomes
four ticks:

\[
\left\lfloor\frac{250}{54.925401}\right\rfloor=4,
\qquad 4\times54.925401=219.701604\text{ ms}
\]

The port combines the three old timers into one message pump:

1. It requests a 1 ms `SetTimer` timer. On Win32s, the classic system timer
   quantizes this to one old system tick per animation event.
2. On Windows versions that export `QueryPerformanceCounter` and
   `QueryPerformanceFrequency`, those functions are resolved dynamically, not
   imported at load time. The message pump is used to check elapsed time, and
   drawing occurs only once per 54.925401 ms logical tick.
3. If the high-resolution counter is unavailable, each delivered timer message
   is treated as one legacy tick. This is the Win32s fallback.
4. Text flashing occurs every four logical ticks, approximately 219.7016 ms,
   matching the old `SetTimer(..., 250, ...)` quantization rather than imposing
   a modern 250 ms interval.

`WM_TIMER` delivery is message-queue dependent. A busy or blocked UI thread can
miss intermediate animation frames; neither Windows 3.x nor Windows 11 can
promise that every visual update is presented at the ideal instant. The QPC
path preserves the average physical tick period when the message loop remains
responsive. It does not busy-wait, change global timer resolution, or use a
multimedia timer.

## Korean text input and output

The menu, dialog, buttons, labels, window title, and other resources are
intentionally **English**, as in the original sample. Only the cake message is
user text; that edit control and the GDI text output use the active Windows ANSI
code page through the `A` APIs.

The source file is ASCII-only. It contains no Korean source literals, so the
compiler's source encoding is irrelevant. The edit control accepts up to 79
characters, with a 321-byte buffer to accommodate multibyte encodings.

| Encoding choice | Advantages | Limitations here |
|---|---|---|
| CP949 / UHC | Native Korean Windows ANSI encoding; works with the ANSI API used by Win32s and Korean Windows 11 | Depends on a Korean ANSI system code page; not portable as a general text-file encoding |
| UTF-8 | Best general interchange and compact source/data representation | Can be passed through the ANSI API only when the active Windows ANSI code page is UTF-8; not a Win32s option |
| UTF-16 | Native representation for modern Windows `W` APIs; independent of the system ANSI code page | The `W` API family is not a common Win32s denominator; using it requires a separate NT-family implementation or a more complex dual A/W code path |

At startup, the program dynamically queries `GetACP` when available:

- ACP 949: Korean text is passed as CP949 bytes.
- ACP 65001: Korean text is passed as UTF-8 bytes, provided the system is
  configured to use UTF-8 as its active ANSI code page (available on supported
  modern Windows versions).
- Any other ACP: the program still runs and the UI remains English, but that
  code page may not represent Korean. For Korean input/output, use a Korean
  Windows system locale (ACP 949) or the UTF-8 system-locale option (ACP 65001).
- If `GetACP` is unavailable, the Win32s fallback assumes a Korean Windows
  installation with CP949.

A Unicode-only Windows 11 build could use UTF-16 and `...W` APIs to support
Korean regardless of the system ACP, but it would no longer be a single common
binary for Win32s. This port deliberately chooses CP949/active-ACP ANSI APIs as
the compatibility baseline.

## Compatibility and resource use

- PE32 / x86 only; not PE32+ / x64.
- Windows 11 x64 runs it through WOW64.
- Static imports are limited to classic `KERNEL32`, `USER32`, and `GDI32` APIs.
- `GetACP`, `QueryPerformanceCounter`, and `QueryPerformanceFrequency` are
  resolved dynamically where present.
- A custom entry point and private pseudo-random generator avoid a dependency
  on the Microsoft C runtime DLLs.
- No C++, COM, common controls, GDI+, DirectX, multimedia timers, or external
  runtime libraries are required by the source.
- The resources use the traditional `MENU` and `DIALOG` forms, not `DIALOGEX`.
- Korean display requires a Korean-capable font; the GDI font request uses
  `HANGUL_CHARSET` when the detected ACP is Korean or UTF-8.

Win32s compatibility is a target, not a verified result. PE linker output,
relocations, subsystem version, and the exact Win32s version still need testing
in a Windows 3.1 + Win32s installation.

## Build with MSVC (32-bit x86)

Use an **x86 Native Tools Command Prompt**. Do not use an x64 target, and do not
define `UNICODE` or `_UNICODE`.

```bat
rc /nologo /fo cake32.res cake32.rc
cl /nologo /TC /c /O1 /W3 /GS- /Zl cake32.c /Fo:cake32.obj
link /nologo /NODEFAULTLIB /MACHINE:X86 /SUBSYSTEM:WINDOWS,3.10 /ENTRY:CakeEntry /BASE:0x00400000 /FIXED /DYNAMICBASE:NO /NXCOMPAT:NO /SAFESEH:NO /STACK:32768,4096 cake32.obj cake32.res kernel32.lib user32.lib gdi32.lib /OUT:CAKE32.exe
```

`/NODEFAULTLIB` and `/Zl` remove the normal CRT startup dependency. The custom
`CakeEntry` calls `WinMain` and exits through `ExitProcess`.

## Build with MinGW-w64 GCC (32-bit i686)

Use `i686-w64-mingw32-gcc`, not the x86-64 compiler.

```sh
i686-w64-mingw32-windres cake32.rc -O coff -o cake32_res.o

i686-w64-mingw32-gcc -std=c89 -Os -Wall -Wextra \
  -ffreestanding -fno-builtin -fno-stack-protector \
  -fno-asynchronous-unwind-tables -fno-unwind-tables -mfpmath=387 \
  -c cake32.c -o cake32.o

i686-w64-mingw32-gcc -nostdlib \
  -Wl,--subsystem,windows \
  -Wl,--major-subsystem-version,3 \
  -Wl,--minor-subsystem-version,10 \
  -Wl,--image-base,0x00400000 \
  -Wl,--disable-dynamicbase \
  -Wl,--disable-nxcompat \
  -Wl,--entry,_CakeEntry \
  cake32.o cake32_res.o -lkernel32 -luser32 -lgdi32 \
  -o CAKE32.exe
```

GNU linker options vary by toolchain version. If an option is not recognized,
remove only that option and inspect the resulting PE headers/imports; keep the
compiler target i686.

## Verify the executable

With MSVC:

```bat
dumpbin /headers CAKE32.exe
dumpbin /imports CAKE32.exe
```

With MinGW:

```sh
i686-w64-mingw32-objdump -f CAKE32.exe
i686-w64-mingw32-objdump -p CAKE32.exe | grep -i 'DLL Name'
```

Expected: PE32, machine i386 (`0x14c`), GUI subsystem, and imports restricted to
`KERNEL32.DLL`, `USER32.DLL`, and `GDI32.DLL`. No `MSVCRT.DLL`, `UCRTBASE.DLL`,
`VCRUNTIME*.DLL`, or `MSVCP*.DLL` dependency is intended.

## References

- Charles Petzold, *Programming Windows*, discussion of Windows timer
  resolution and the 54.925 ms PC timer tick:
  <https://www.pcjs.org/documents/books/mspl13/win/prgwin/>
- Charles Petzold, *Windows 1.0
and the
Applications of Tomorrow
By Charles Petzold
A Keynote Delivered at DevConnections in Las Vegas, November 7, 2005:
<https://www.charlespetzold.com/etcetera/Windows1/index.html>
- Microsoft Learn, [About timers](https://learn.microsoft.com/en-us/windows/win32/winmsg/about-timers)
- Microsoft Learn, [SetTimer](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-settimer)
- Microsoft Learn, [Unicode in the Windows API](https://learn.microsoft.com/en-us/windows/desktop/Intl/unicode-in-the-windows-api)
