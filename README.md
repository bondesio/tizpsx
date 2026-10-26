# TizPSX

> **This is a native Tizen app, not a web app.** TizPSX is a 32-bit ARM ELF written in C/C++ against
> EFL/Evas and `capi-media-audio-io`, packaged as a signed **TPK**. It is *not* an HTML5/WGT web application and
> no web engine is involved in emulation. The only WGT in this repository (`launcher/`) is a Home-rail tile that
> starts the native app.
>
> It runs on **one rooted Samsung QE75Q60AAUXXC** (Tizen 6.0, ARMv7) and depends on the research that made that possible:
>
> - [Samsung QE75Q60AAUXXC root research](https://github.com/bondesio/qe75q60aa-root-research): how the TV was rooted, how native packages are installed and how the root helper services work.
> - [Native Tizen 6 apps and UEP](https://github.com/bondesio/tizen6-native-app-uep-research): why a native process starts under User Execution Prevention but only renders its UI once UEP is disabled.
>
> This is a research record and source release, not an installer for stock TVs. No game ROMs, BIOS images, saves or
> Samsung binaries are included; link libraries are copied from your own TV at build time (see *Build*).
> Nothing here has been published as a ready-to-install package.

**Sibling projects**

- [Tizen emulator starter kit](https://github.com/bondesio/tizen-emulator-framework): Shared conventions, snippets and host-test fakes for the native Tizen TV emulators
- [TizNES](https://github.com/bondesio/tiznes): Native Tizen 6 NES emulator (InfoNES) for a rooted Samsung Q60A
- [TizGB](https://github.com/bondesio/tizgb): Native Tizen 6 Game Boy / Game Boy Color emulator (Walnut-CGB) for a rooted Samsung Q60A
- [TizGBA](https://github.com/bondesio/tizgba): Native Tizen 6 Game Boy Advance emulator (mGBA) for a rooted Samsung Q60A
- [TizNeo](https://github.com/bondesio/tizneo): Native Tizen 6 Neo Geo AES/MVS emulator (Geolith) for a rooted Samsung Q60A

A native PlayStation emulator project for the Samsung Q60A (Tizen 6.0,
32-bit ARM userspace), following the shared Tizen emulator framework and the
TizNES/TizGB app conventions.

Version 0.2.12 is installed on the Q60A and has booted the staged Crash
Bandicoot disc to the emulation worker. Video settings provide saved
controls for internal resolution (1x, 2x, 3x, 4x) and
optional edge smoothing. The existing NEON path provides optimized 2x
enhancement. Experimental 3x/4x modes use a scalar high-resolution rasterizer
for triangles, sprites, lines and fills; these render more polygon coverage,
while source textures remain at native detail. Wrapped scanouts, RGB24 and
games already using high-resolution modes fall back to native resolution for
memory safety. The edge-aware smoothing pass blends pixels around strong local
luminance transitions before Evas presentation. It is separate from the
Nearest/Bilinear output filter and Integer/Fit screen sizing. The selected
internal resolution is saved and takes effect on the next game launch. 3x/4x
performance and visual compatibility remain unverified on the TV.

The frontend follows the
shared TizNES/TizGB/NeoGeo menu layout. Controller Settings provide exclusive
capture, two player assignments, per-control mapping for all 16 DualShock
buttons, a guided input test, analog sticks, and a rumble toggle. A paced
worker runs PCSX ReARMed, queues audio, and converts completed frames; the UI
thread polls input and uploads frames to Evas. See [STATUS.md](STATUS.md) for on-device validation and hardware limits.
The separate Home launcher WGT remains installed and registered in the TV's
Apps list. Pinning it to the Home app bar is a separate TV UI action.

No BIOS, firmware, disc image, or other copyrighted console/game data is
bundled, fetched, or committed. Users must supply files they are authorized to
use. The emulator currently supports user-supplied `.cue`, `.bin`, `.iso`,
`.img`, `.chd`, `.pbp`, and `.m3u` paths and requires a recognized 512 KiB BIOS.
Memory cards go under the app's private `data/saves/` directory.

## Local build

From this directory:

```sh
./tools/build-local.sh
```

This creates and signs the native TPK and Home launcher WGT with the local
`tvdev` profile. It uses the existing Tizen ARM compiler and cached Tizen UI
libraries. On the first build, it also uses read-only SSH access to the Q60A
to fetch its `libdl.so.2`, `libpthread.so.0`, and `libm.so.6` link inputs. PCSX ReARMed's
Makefile is invoked for ARMv7/NEON/softfp, with asynchronous worker threads
disabled. The helper uses locally installed ARM glibc headers because the
Tizen compiler's embedded header sysroot is not readable on this workstation;
the final executable links against the TV's cached libraries.

Build outputs:

- `app/out/org.pablo.tizpsx-0.2.12-arm.tpk`
- `launcher/out/TIZPSX0001-1.0.0.wgt`

The current UI supports keyboard/TV-remote fallback for directions, Cross,
Circle, Start, and Back. It prefers physical gamepads over Tizen's virtual pad
for a new Player 1 assignment. Rumble is sent through Linux force feedback when
the selected device supports it; the currently connected USB pad reports no
force-feedback capability. Gameplay performance and memory-card writes still
need hands-on validation; see [STATUS.md](STATUS.md).

## License

The frontend code in this repository is released under the Unlicense (see `LICENSE`). It links the GPL-2.0 PCSX ReARMed core (`vendor/pcsx_rearmed/`, see `COPYING` and its `NOTICE`), so **a built TizPSX binary is a combined work distributed under GPL-2.0** and its corresponding source is this repository. Other third-party code keeps its own license.
