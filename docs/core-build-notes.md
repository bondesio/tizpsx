# PCSX ReARMed core integration notes

## Source provenance and license

- Upstream: `https://github.com/libretro/pcsx_rearmed`
- Revision: `c8816799b50388e61cfe237fe2cdbb7d8175f20a`
- Git tree: `bea7c3942c7e2039cc70d39b1f74b5484ef265cf`
- Upstream license: GPL-2.0; see `vendor/pcsx_rearmed/COPYING` and
  `vendor/pcsx_rearmed/AUTHORS`. `vendor/pcsx_rearmed/NOTICE` records the
  provenance and content boundary.
- Pinned upstream submodule `notaz/libpicofe`: commit
  `a2697389e76744cfa0c56c13a172845c45ede719`, tree
  `9843389285213e1b6883b9a7237c79faf99ff3b4`; its license notice is in
  `vendor/pcsx_rearmed/frontend/libpicofe/README`.
- This source was fetched from upstream and inspected. The upstream
  `Makefile.libretro` was used for a static ARM archive; no upstream configure
  or install scripts were run. No tests were run.

## Adapter contract

`app/core/pcsx_rearmed_adapter.c` implements the `psx_core_*` API from
`app/psx_core.h` by calling PCSX ReARMed's libretro entry points. It accepts
recognized 512 KiB BIOS images and user supplied CUE, BIN, ISO, IMG, CHD, PBP,
and M3U paths. The BIOS is exposed to the core through a private temporary
directory symlink named `scph1001.bin`; no firmware bytes are copied into the
project. Game paths are passed directly to the upstream loader.

Input masks use Libretro's conventional PlayStation mapping (B/Cross,
Y/Square, A/Circle, X/Triangle, shoulders, directions, Start and Select).
Video is accepted only as RGB565 within the public frame bounds. Audio batches
are copied to the public 44.1-kHz stereo buffer up to its capacity; excess
samples in a frame are dropped. The adapter retains the prior video image when
the core requests frame duplication. The temporary private directory contains
the BIOS alias; core memory-card files use the app-private `data/saves/`
directory.

## Integration and validation constraints

- `app/Makefile` builds PCSX ReARMed's libretro sources into a static ARM
  archive and links it with the adapter into the native TPK. It disables
  asynchronous CD, GPU, SPU and dynarec worker threads for the first port.
- The Tizen cross-compiler's configured glibc header sysroot points into an
  unreadable `/root/...` build path on the build workstation. The local build uses installed
  ARMHF headers plus a soft-float stub header, and links against the TV's
  cached libc/EFL/audio libraries. `app/core/stat_compat.c` bridges public
  `stat`/`fstat` aliases to the GLIBC 2.4 `__xstat`/`__fxstat` exports present
  on the TV, and supplies the newer host-header `__isoc23_strtol` alias.
- The TV's `libdl.so.2` and `libpthread.so.0` were fetched read-only for local
  linking. Their runtime availability is supported by the Q60A filesystem
  observation, but the built app has not been run there.
- The Q60A target is 32-bit ARM/Tizen 6.0. Upstream ARM dynarec memory mapping,
  executable-memory policy, required compiler flags, and runtime performance
  have not been verified on the TV. Do not claim the dynarec or full-speed
  gameplay works until the build and device behavior are validated.
- The local signed TPK and ARM ELF compile. The selected image loader paths,
  BIOS recognition, audio rate, frame bounds, save writes and clean failure
  behavior have not been exercised with real content or on the TV.
- Upstream `retro_init()` calls `exit(1)` if its internal core initialization
  fails. The adapter prevalidates paths and BIOS shape, but cannot convert that
  internal failure into a recoverable `psx_core_init()` return without a
  targeted upstream API change.
