#ifndef PCSX_GPU_NEON_PUBLIC_H
#define PCSX_GPU_NEON_PUBLIC_H

#ifdef __cplusplus
extern "C" {
#endif

/* Call before retro_init/GPU initialization. Values clamp to 2..4 (default
 * 2); choose 1x by disabling enhancement in the frontend. Runtime changes
 * require a complete GPU shutdown/reinitialization. Scales 3/4 currently use
 * an experimental scalar rasterizer with unvalidated performance/edge parity.
 * Wrapped scanouts fall back to native output; its actual geometry and a
 * GPU_NEON log message signal the fallback. RGB24/hires retain native fallback.
 */
void GPUsetResolutionScale(unsigned multiplier);

/* Input pixel pitch for the current scanout, including native fallback. */
unsigned GPUgetEnhancementPitch(void);

#ifdef __cplusplus
}
#endif
#endif
