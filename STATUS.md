# TizPSX — status

## Current state


- **TizPSX 0.3.0 installed (2026-10-26, not launched):** library = Exit TizPSX,
  games, Settings (first game selected); Settings → System → **Performance
  overlay** Off / FPS / Detailed, persisted as `perf_overlay` in `settings.ini`,
  fed by a per-second snapshot from the emulation worker (late = frame finished
  after its deadline). The RGB565 conversion and edge-smoothing pass moved to
  `app/ui/video_convert.h`: bit-exact with 0.2.12 (`tools/test_video_convert.c`,
  9 sizes x 4 contents, ASan/UBSan), 2.7x faster on the host. This targets the
  37.2 FPS / 25.6 ms-per-frame sample at 2x + edge smoothing, **not yet re-timed
  on the TV**; use the Detailed overlay (work ms, late frames) to check. BSS is
  26,078,512 bytes (+256), so the dynarec mapping is unchanged. Signed TPK
  SHA-256 `915702406b7b1a643d8fed881d0dd7ff972c32f1fcf2ee280d1f574cbd750210`;
  installed ELF `bba5f0200c7ce6bfb041b26eae084bffc7e3923cac3378812ace88a3a5d66846`.
  `harness/` runs the real `psx_main.c` (worker thread, conversion, smoothing)
  against fake EFL/input/library/core: 20 checks, ASan/UBSan clean. Rollback:
  `backups/2026-10-26-before-0.3.0-deploy/`.

- **TizPSX 0.2.12 installed and Crash Bandicoot booted on the Q60A
  (2026-10-24).** Fixed the 0.2.10 dynarec abort: static frame buffers had
  moved ARM's generated-code mapping 46.7 MiB from a branch target, beyond its
  32 MiB range. Frame buffers are now allocated after core initialization;
  executable BSS fell from 61 MiB to 25 MiB and the dynarec mapping is now at
  `0x00d9c000`. Crash Bandicoot loaded from its CUE and the worker stayed at
  about 60 FPS in early samples. A later run with the saved 2x + Edge smoothing
  settings measured 37.2 FPS, 38 late frames and 25.6 ms/frame conversion, so
  smooth gameplay is not yet verified. The expensive smoothing path is a likely
  contributor but has not been isolated. Input, sound, the on-screen result,
  and 3x/4x visual/performance behavior still need Pablo's hands-on check.
  TPK SHA-256
  `e1af68d35fa67bc20bd609ea194fa2fa7fed19812e0e0cc68d56840b2aba0f8d`;
  installed ELF SHA-256
  `137e9a6262361b381bb10e0c401930ee05346712a639f74d2aab1ebf8eb09f95`.
  Settings and controller-mapping hashes match the pre-deploy snapshot.
  Rollback snapshots: `backups/2026-10-24-before-0.2.12-deploy/` and
  `backups/2026-10-24-before-0.2.12-manifest-reinstall/`.
- **Core:** PCSX ReARMed at upstream revision
  `c8816799b50388e61cfe237fe2cdbb7d8175f20a` (GPL-2.0), with pinned libpicofe.
  ARMv7/NEON/softfp build succeeds against the local cross toolchain. Region
  timing is read from the core and frame calls are paced at that rate.
- **Media and saves:** no BIOS or games are included. A user-supplied recognized
  512 KiB BIOS is required. CUE/BIN/ISO/IMG/CHD/PBP/M3U paths are scanned from
  app data and mounted storage. Core memory cards are directed to app-private
  `data/saves/`; this path is build-verified but has not been exercised on TV.
- **Input:** all first-edition DualShock digital controls are mapped, including
  L2/R2/L3/R3; left/right stick values reach PCSX ReARMed's analog input API.
  Exclusive capture is active on the USB gamepad. Input capture and mappings
  should still be checked hands-on with the controller; save states and
  user-selectable region are not implemented.
- **Unverified on-device behavior:** visible game presentation and sound,
  controller interaction, and memory-card writes. Crash Bandicoot now runs
  through PCSX's dynarec on the TV. The app is user-space only; kernel, modules,
  firmware and boot chain are out of scope.
- Project is a local Git repository on `main`, no remote. The emulator follows
  the shared `emulator-framework/` kit and TizNES/TizGB app conventions.

## Immediate next actions

- Pablo to confirm Crash Bandicoot's on-screen image, sound, and controller
  input. Then check the 1x–4x options and edge smoothing, especially 3x/4x
  frame pacing and visual quality.
