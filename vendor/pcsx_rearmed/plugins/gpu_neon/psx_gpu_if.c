/*
 * (C) Gražvydas "notaz" Ignotas, 2011
 *
 * This work is licensed under the terms of any of these licenses
 * (at your option):
 *  - GNU GPL, version 2 or later.
 *  - GNU LGPL, version 2.1 or later.
 * See the COPYING file in the top-level directory.
 */

#include <stdio.h>
#include <assert.h>
#include "../../include/compiler_features.h"

#define ARRAY_SIZE(x) (sizeof(x) / sizeof((x)[0]))
#ifndef min
#define min(a, b) ((a) < (b) ? (a) : (b))
#endif
#ifndef max
#define max(a, b) ((a) > (b) ? (a) : (b))
#endif

extern const unsigned char cmd_lengths[256];
#define command_lengths cmd_lengths

static int initialized;
static unsigned resolution_scale = 2;
static unsigned enhancement_alloc_size;

#define PCSX

static noinline void sync_enhancement_buffers(int x, int y, int w, int h);

#include "../gpulib/gpu.h"
#include "gpu_neon.h"
#include "psx_gpu/psx_gpu.c"
#include "psx_gpu/psx_gpu_parse.c"

static psx_gpu_struct egpu __attribute__((aligned(256)));

void GPUsetResolutionScale(unsigned multiplier)
{
  /* Buffer geometry is fixed for one GPU lifetime. The frontend calls this
   * before retro_init; native rendering is selected by disabling enhancement. */
  if (initialized) {
    SysPrintf("GPUsetResolutionScale requires an uninitialized GPU\n");
    return;
  }
  resolution_scale = multiplier < 2 ? 2 : multiplier > 4 ? 4 : multiplier;
}

unsigned GPUgetEnhancementPitch(void)
{
  return gpu.state.enhancement_active && egpu.enhancement_pitch ?
    egpu.enhancement_pitch : 1024;
}

int renderer_do_cmd_list(uint32_t *list, int count, uint32_t *ex_regs,
 int *cycles_sum, int *cycles_last, int *last_cmd)
{
  int ret;

  if (gpu.state.enhancement_active)
    ret = gpu_parse_enhanced(&egpu, list, count * 4, ex_regs,
            cycles_sum, cycles_last, (u32 *)last_cmd);
  else
    ret = gpu_parse(&egpu, list, count * 4, ex_regs,
            cycles_sum, cycles_last, (u32 *)last_cmd);

  ex_regs[1] &= ~0x1ff;
  ex_regs[1] |= egpu.texture_settings & 0x1ff;
  return ret;
}

#define ENHANCEMENT_BUF_SIZE (1024 * 1024 * 2 * 4 + 4096 * 2)

static void *get_enhancement_bufer(int *x, int *y, int *w, int *h,
 int *vram_h)
{
  if (resolution_scale > 2 &&
      (*x < 0 || *y < 0 || *w < 0 || *h < 0 ||
       *x + *w > 1024 || *y + *h > 512))
    return NULL;
  uint16_t *ret = select_enhancement_buf_ptr(&egpu, *x, *y);
  if (ret == NULL)
    return NULL;

  *x *= resolution_scale;
  *y *= resolution_scale;
  *w *= resolution_scale;
  *h *= resolution_scale;
  *vram_h = 512 * resolution_scale;
  return ret;
}

static void map_enhancement_buffer(void)
{
  // Preserve the 2x renderer's four 1024*1024 buffers. The scalar 3x/4x
  // path uses four full scaled VRAM surfaces, with independent wide pitch.
  enhancement_alloc_size = resolution_scale == 2 ? ENHANCEMENT_BUF_SIZE :
    1024 * 512 * resolution_scale * resolution_scale * 2 * 4 + 4096 * 2;
  egpu.enhancement_buf_ptr = gpu.mmap(enhancement_alloc_size);
  if (egpu.enhancement_buf_ptr == NULL || egpu.enhancement_buf_ptr == (void *)(intptr_t)-1) {
    SysPrintf("failed to map enhancement buffer\n");
    egpu.enhancement_buf_ptr = NULL;
    gpu.get_enhancement_bufer = NULL;
  }
  else {
    egpu.enhancement_buf_ptr += 4096 / 2;
    gpu.get_enhancement_bufer = get_enhancement_bufer;
  }
}

int renderer_init(void)
{
  egpu.enhancement_scale = gpu.enhancement_scale = resolution_scale;
  egpu.enhancement_pitch = gpu.enhancement_pitch =
    resolution_scale == 2 ? 1024 : 1024 * resolution_scale;
  if (gpu.vram != NULL) {
    initialize_psx_gpu(&egpu, gpu.vram);
    initialized = 1;
  }

  if (resolution_scale > 2)
    SysPrintf("GPU_NEON: experimental scalar %ux enhancement rasterizer\n",
      resolution_scale);

  if (gpu.mmap != NULL && egpu.enhancement_buf_ptr == NULL)
    map_enhancement_buffer();

  return 0;
}

void renderer_finish(void)
{
  if (egpu.enhancement_buf_ptr != NULL) {
    egpu.enhancement_buf_ptr -= 4096 / 2;
    gpu.munmap(egpu.enhancement_buf_ptr, enhancement_alloc_size);
  }
  egpu.enhancement_buf_ptr = NULL;
  egpu.enhancement_current_buf_ptr = NULL;
  initialized = 0;
  gpu.get_enhancement_bufer = NULL;
}

static noinline void sync_enhancement_buffers(int x, int y, int w, int h)
{
  int i, right = x + w, bottom = y + h;
  const u16 *src = gpu.vram;
  // use these because the scanout struct may hold reduced w, h
  // due to intersection stuff, see the update_enhancement_buf_scanouts() mess
  int s_w = max(gpu.screen.hres, gpu.screen.w);
  int s_h = gpu.screen.vres;
  if (gpu.screen.y < 0)
    s_h -= gpu.screen.y;
  s_w = min(s_w, 512);
  for (i = 0; i < ARRAY_SIZE(egpu.enhancement_scanouts); i++) {
    const struct psx_gpu_scanout *s = &egpu.enhancement_scanouts[i];
    u16 *dst = select_enhancement_buf_by_index(&egpu, i);
    int x1, x2, y1, y2;
    if (s->w == 0) continue;
    if (s->x >= right) continue;
    if (s->x + s_w <= x) continue;
    if (s->y >= bottom) continue;
    if (s->y + s_h <= y) continue;
    x1 = max(x, s->x);
    x2 = min(right, s->x + s_w);
    y1 = max(y, s->y);
    y2 = min(bottom, s->y + s_h);
    if (resolution_scale > 2) {
      int xx, yy, sy, sx;
      x1 = max(x1, 0); x2 = min(x2, 1024);
      y1 = max(y1, 0); y2 = min(y2, 512);
      for (yy = y1; yy < y2; yy++) {
        for (sy = 0; sy < resolution_scale; sy++) {
          u16 *row = dst + (yy * resolution_scale + sy) * egpu.enhancement_pitch;
          for (xx = x1; xx < x2; xx++)
            for (sx = 0; sx < resolution_scale; sx++)
              row[xx * resolution_scale + sx] = src[yy * 1024 + xx];
        }
      }
      continue;
    }
    // 16-byte align for the asm version
    x2 += x1 & 7;
    x1 &= ~7;
    scale2x_tiles8(dst + y1 * 1024*2 + x1 * 2,
        src + y1 * 1024 + x1, (x2 - x1 + 7) / 8u, y2 - y1);
  }
}

void renderer_sync_ecmds(uint32_t *ecmds)
{
  s32 dummy0 = 0;
  u32 dummy1 = 0;
  gpu_parse(&egpu, ecmds + 1, 6 * 4, ecmds, &dummy0, &dummy0, &dummy1);
}

void renderer_update_caches(int x, int y, int w, int h, int state_changed)
{
  update_texture_cache_region(&egpu, x, y, x + w - 1, y + h - 1);

  if (gpu.state.enhancement_active) {
    if (state_changed) {
      int vres = gpu.screen.vres;
      if (gpu.screen.y < 0)
        vres -= gpu.screen.y;
      memset(egpu.enhancement_scanouts, 0, sizeof(egpu.enhancement_scanouts));
      egpu.enhancement_scanout_eselect = 0;
      update_enhancement_buf_scanouts(&egpu,
        gpu.screen.src_x, gpu.screen.src_y, gpu.screen.hres, vres);
      return;
    }
    if (resolution_scale > 2) {
      int w1 = min(w, 1024 - x), h1 = min(h, 512 - y);
      sync_enhancement_buffers(x, y, w1, h1);
      if (w1 < w) sync_enhancement_buffers(0, y, w - w1, h1);
      if (h1 < h) sync_enhancement_buffers(x, 0, w1, h - h1);
      if (w1 < w && h1 < h)
        sync_enhancement_buffers(0, 0, w - w1, h - h1);
    }
    else
      sync_enhancement_buffers(x, y, w, h);
  }
}

void renderer_flush_queues(void)
{
  flush_render_block_buffer(&egpu);
}

void renderer_set_interlace(int enable, int is_odd)
{
  egpu.render_mode &= ~(RENDER_INTERLACE_ENABLED|RENDER_INTERLACE_ODD);
  if (enable)
    egpu.render_mode |= RENDER_INTERLACE_ENABLED;
  if (is_odd)
    egpu.render_mode |= RENDER_INTERLACE_ODD;
}

void renderer_notify_screen_change(const struct psx_gpu_screen *screen)
{
  int x = screen->src_x, y = screen->src_y;
  int vres = screen->vres;
  if (!gpu.state.enhancement_active || !egpu.enhancement_buf_ptr)
    return;

  if (screen->y < 0)
    vres -= screen->y;
  update_enhancement_buf_scanouts(&egpu, x, y, screen->hres, vres);
}

#include "../../frontend/plugin_lib.h"

void renderer_set_config(const struct rearmed_cbs *cbs)
{
  egpu.enhancement_scale = gpu.enhancement_scale = resolution_scale;
  egpu.enhancement_pitch = gpu.enhancement_pitch =
    resolution_scale == 2 ? 1024 : 1024 * resolution_scale;
  if (!initialized) {
    initialize_psx_gpu(&egpu, gpu.vram);
    initialized = 1;
  }
  if (cbs->pl_set_gpu_caps)
    cbs->pl_set_gpu_caps(GPU_CAP_SUPPORTS_2X);
  
  egpu.allow_dithering = cbs->dithering;
  egpu.force_dithering = cbs->dithering >> 1;
  /*
  if (!egpu.allow_dithering) {
    egpu.dither_table[0] = dither_table_row(0, 0, 0, 0);
    egpu.dither_table[1] = dither_table_row(0, 0, 0, 0);
    egpu.dither_table[2] = dither_table_row(0, 0, 0, 0);
    egpu.dither_table[3] = dither_table_row(0, 0, 0, 0);
  } else {
    egpu.dither_table[0] = dither_table_row(-4, 0, -3, 1);
    egpu.dither_table[1] = dither_table_row(2, -2, 3, -1);
    egpu.dither_table[2] = dither_table_row(-3, 1, -4, 0);
    egpu.dither_table[3] = dither_table_row(3, -1, 2, -2); 
  }
  */

  egpu.hack_disable_main = cbs->gpu_neon.enhancement_no_main;
  egpu.hack_texture_adj = cbs->gpu_neon.enhancement_tex_adj;
  if (gpu.state.enhancement_enable) {
    if (gpu.mmap != NULL && egpu.enhancement_buf_ptr == NULL)
      map_enhancement_buffer();
  }
}

// vim:ts=2:sw=2:expandtab
