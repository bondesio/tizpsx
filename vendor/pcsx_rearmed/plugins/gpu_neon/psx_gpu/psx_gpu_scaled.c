/*
 * Experimental 3x/4x enhancement rasterizer, GPL-2.0-or-later.
 *
 * Included by psx_gpu_parse.c. Native VRAM, command decoding, texture caches,
 * timing and the optimized 2x renderer retain their original code paths.
 * Evaluate primitive coverage and affine attributes on the enhanced grid;
 * this does not resize an image produced by the 2x renderer.
 *
 * Keep this scalar reference path separate from ARM's fixed-pitch kernels.
 * It needs visual regression and performance validation before being treated
 * as equivalent to the mature 2x implementation, especially at shared edges.
 */

static int scaled_clip(psx_gpu_struct *g, int *x0, int *y0, int *x1, int *y1)
{
  int scale = g->enhancement_scale;
  int left = max(0, g->saved_viewport_start_x);
  int top = max(0, g->saved_viewport_start_y);
  int right = min(1023, g->saved_viewport_end_x);
  int bottom = min(511, g->saved_viewport_end_y);
  right = min(right, left + 511);
  *x0 = max(*x0, left * scale);
  *y0 = max(*y0, top * scale);
  *x1 = min(*x1, (right + 1) * scale - 1);
  *y1 = min(*y1, (bottom + 1) * scale - 1);
  return g->enhancement_current_buf_ptr && *x0 <= *x1 && *y0 <= *y1;
}

static int scaled_field_visible(const psx_gpu_struct *g, int y)
{
  if (!(g->render_mode & RENDER_INTERLACE_ENABLED))
    return 1;
  /* All subrows of one native scanline belong to the same field, including
   * at the odd multiplier 3. Do not interpret enhanced row parity as a field. */
  return ((y / g->enhancement_scale) & 1) ==
    !!(g->render_mode & RENDER_INTERLACE_ODD);
}

static int scaled_clamp(int value, int high)
{
  return value < 0 ? 0 : value > high ? high : value;
}

static void render_fill_scaled(psx_gpu_struct *g, int x, int y, int w, int h,
 unsigned color)
{
  int i, nx, ny, sx, sy, scale = g->enhancement_scale;
  int screen_w = min(max(gpu.screen.hres, gpu.screen.w), 512);
  int screen_h = gpu.screen.vres - min(gpu.screen.y, 0);
  u16 pixel = ((color & 255) >> 3) | (((color >> 8) & 255) >> 3 << 5) |
    (((color >> 16) & 255) >> 3 << 10);
  for (i = 0; i < ARRAY_SIZE(g->enhancement_scanouts); i++) {
    const struct psx_gpu_scanout *s = &g->enhancement_scanouts[i];
    u16 *dest = select_enhancement_buf_by_index(g, i);
    int left = max(x, s->x), right = min(x + w, s->x + screen_w);
    int top = max(y, s->y), bottom = min(y + h, s->y + screen_h);
    if (!s->w) continue;
    left = max(left, 0); right = min(right, 1024);
    top = max(top, 0); bottom = min(bottom, 512);
    for (ny = top; ny < bottom; ny++) {
      if (!scaled_field_visible(g, ny * scale)) continue;
      for (sy = 0; sy < scale; sy++) {
        u16 *row = dest + (ny * scale + sy) * g->enhancement_pitch;
        for (nx = left; nx < right; nx++)
          for (sx = 0; sx < scale; sx++) row[nx * scale + sx] = pixel;
      }
    }
  }
}

static u16 scaled_texel(const psx_gpu_struct *g, int u, int v)
{
  unsigned mode = (g->texture_settings >> 7) & 3;
  unsigned page = g->current_texture_page;
  unsigned tw = g->texture_window_settings;
  unsigned mask_u = (tw & 31) << 3, mask_v = ((tw >> 5) & 31) << 3;
  unsigned x, y, word, index;
  /* Apply the hardware texture window directly. Addresses remain native;
   * no enhanced framebuffer is ever used as a texture source. */
  u = ((u & 255) & ~mask_u) | (((tw >> 10) << 3) & mask_u);
  v = ((v & 255) & ~mask_v) | (((tw >> 15) << 3) & mask_v);
  x = (page & 15) * 64;
  y = (((page >> 4) * 256 + v) & 511) * 1024;
  if (mode == TEXTURE_MODE_4BPP) {
    word = g->vram_ptr[y + ((x + ((unsigned)u >> 2)) & 1023)];
    index = (word >> ((u & 3) * 4)) & 15;
  }
  else if (mode == TEXTURE_MODE_8BPP) {
    word = g->vram_ptr[y + ((x + ((unsigned)u >> 1)) & 1023)];
    index = (word >> ((u & 1) * 8)) & 255;
  }
  else
    return g->vram_ptr[y + ((x + u) & 1023)];
  return g->vram_ptr[((unsigned)g->clut_settings * 16 + index) & 0x7ffff];
}

static void scaled_pixel(psx_gpu_struct *g, int x, int y, unsigned flags,
 int r, int green, int b, int u, int v, int dither)
{
  static const signed char matrix[4][4] = {
    { -4, 0, -3, 1 }, { 2, -2, 3, -1 },
    { -3, 1, -4, 0 }, { 3, -1, 2, -2 }
  };
  u16 *dest = g->enhancement_current_buf_ptr + y * g->enhancement_pitch + x;
  unsigned texel = 0, tex_msb = 0;
  int textured = flags & RENDER_FLAGS_TEXTURE_MAP;
  int blend = flags & RENDER_FLAGS_BLEND;
  int offset = dither ? matrix[y & 3][x & 3] : 0;
  if ((g->render_state_base & RENDER_STATE_MASK_EVALUATE) && (*dest & 0x8000))
    return;
  if (textured) {
    texel = scaled_texel(g, u, v);
    if (!texel)
      return;
    tex_msb = texel & 0x8000;
    blend = blend && tex_msb;
    if (flags & RENDER_FLAGS_MODULATE_TEXELS) {
      r = texel & 31;
      green = (texel >> 5) & 31;
      b = (texel >> 10) & 31;
    }
    else {
      r = scaled_clamp(((texel & 31) * r >> 4) + offset, 255) >> 3;
      green = scaled_clamp((((texel >> 5) & 31) * green >> 4) + offset, 255) >> 3;
      b = scaled_clamp((((texel >> 10) & 31) * b >> 4) + offset, 255) >> 3;
    }
  }
  else {
    r = scaled_clamp(r + offset, 255) >> 3;
    green = scaled_clamp(green + offset, 255) >> 3;
    b = scaled_clamp(b + offset, 255) >> 3;
  }
  if (blend) {
    int dr = *dest & 31, dg = (*dest >> 5) & 31, db = (*dest >> 10) & 31;
    switch ((g->render_state_base >> 6) & 3) {
    case BLEND_MODE_AVERAGE:
      r = (dr + r) >> 1; green = (dg + green) >> 1; b = (db + b) >> 1;
      break;
    case BLEND_MODE_ADD:
      r += dr; green += dg; b += db;
      break;
    case BLEND_MODE_SUBTRACT:
      r = dr - r; green = dg - green; b = db - b;
      break;
    default:
      r = dr + (r >> 2); green = dg + (green >> 2); b = db + (b >> 2);
      break;
    }
  }
  *dest = scaled_clamp(r, 31) | (scaled_clamp(green, 31) << 5) |
    (scaled_clamp(b, 31) << 10) | tex_msb | g->mask_msb;
}

static s64 scaled_edge(int ax, int ay, int bx, int by, int x, int y)
{
  return (s64)(bx - ax) * (y - ay) - (s64)(by - ay) * (x - ax);
}

static int scaled_top_left(int ax, int ay, int bx, int by)
{
  return by < ay || (by == ay && bx > ax);
}

static void render_triangle_scaled(psx_gpu_struct *g, vertex_struct *vertices[3],
 unsigned flags)
{
  vertex_struct *a = vertices[0], *b = vertices[1], *c = vertices[2], *tmp;
  int scale = g->enhancement_scale;
  int ax, ay, bx, by, cx, cy, x0, y0, x1, y1, x, y, i;
  int inclusive[3], attrs[3][5];
  s64 area, row_edge[3], edge_dx[3], edge_dy[3];
  s64 attr_dx[5], attr_dy[5], row_attr[5];
  unsigned color = g->triangle_color;
  if (!g->enhancement_current_buf_ptr)
    return;
  flush_render_block_buffer(g);
  area = scaled_edge(a->x, a->y, b->x, b->y, c->x, c->y);
  if (!area)
    return;
  if (area < 0) {
    tmp = b; b = c; c = tmp;
  }
  ax = a->x * scale; ay = a->y * scale;
  bx = b->x * scale; by = b->y * scale;
  cx = c->x * scale; cy = c->y * scale;
  area = scaled_edge(ax, ay, bx, by, cx, cy);
  x0 = min(ax, min(bx, cx)); y0 = min(ay, min(by, cy));
  x1 = max(ax, max(bx, cx)); y1 = max(ay, max(by, cy));
  if (!scaled_clip(g, &x0, &y0, &x1, &y1))
    return;
  inclusive[0] = scaled_top_left(bx, by, cx, cy);
  inclusive[1] = scaled_top_left(cx, cy, ax, ay);
  inclusive[2] = scaled_top_left(ax, ay, bx, by);
  row_edge[0] = scaled_edge(bx, by, cx, cy, x0, y0);
  row_edge[1] = scaled_edge(cx, cy, ax, ay, x0, y0);
  row_edge[2] = scaled_edge(ax, ay, bx, by, x0, y0);
  edge_dx[0] = by - cy; edge_dx[1] = cy - ay; edge_dx[2] = ay - by;
  edge_dy[0] = cx - bx; edge_dy[1] = ax - cx; edge_dy[2] = bx - ax;
  attrs[0][0] = a->u; attrs[0][1] = a->v;
  attrs[1][0] = b->u; attrs[1][1] = b->v;
  attrs[2][0] = c->u; attrs[2][1] = c->v;
  attrs[0][2] = a->r; attrs[0][3] = a->g; attrs[0][4] = a->b;
  attrs[1][2] = b->r; attrs[1][3] = b->g; attrs[1][4] = b->b;
  attrs[2][2] = c->r; attrs[2][3] = c->g; attrs[2][4] = c->b;
  for (i = 0; i < 5; i++) {
    attr_dx[i] = (edge_dx[0] * attrs[0][i] + edge_dx[1] * attrs[1][i] +
      edge_dx[2] * attrs[2][i]) * 65536 / area;
    attr_dy[i] = (edge_dy[0] * attrs[0][i] + edge_dy[1] * attrs[1][i] +
      edge_dy[2] * attrs[2][i]) * 65536 / area;
    row_attr[i] = (row_edge[0] * attrs[0][i] + row_edge[1] * attrs[1][i] +
      row_edge[2] * attrs[2][i]) * 65536 / area + 32768;
  }
  for (y = y0; y <= y1; y++) {
    s64 e[3] = { row_edge[0], row_edge[1], row_edge[2] }, at[5];
    for (i = 0; i < 5; i++) at[i] = row_attr[i];
    if (scaled_field_visible(g, y))
      for (x = x0; x <= x1; x++) {
        if ((e[0] > 0 || (!e[0] && inclusive[0])) &&
            (e[1] > 0 || (!e[1] && inclusive[1])) &&
            (e[2] > 0 || (!e[2] && inclusive[2]))) {
          int r = color & 255, green = (color >> 8) & 255, blue = (color >> 16) & 255;
          if (flags & RENDER_FLAGS_SHADE) {
            r = scaled_clamp(at[2] >> 16, 255);
            green = scaled_clamp(at[3] >> 16, 255);
            blue = scaled_clamp(at[4] >> 16, 255);
          }
          scaled_pixel(g, x, y, flags, r, green, blue, at[0] >> 16, at[1] >> 16,
            !!(g->render_state_base & RENDER_STATE_DITHER));
        }
        for (i = 0; i < 3; i++) e[i] += edge_dx[i];
        for (i = 0; i < 5; i++) at[i] += attr_dx[i];
      }
    for (i = 0; i < 3; i++) row_edge[i] += edge_dy[i];
    for (i = 0; i < 5; i++) row_attr[i] += attr_dy[i];
  }
}

static void render_sprite_scaled(psx_gpu_struct *g, int x, int y, unsigned u,
 unsigned v, unsigned width, unsigned height, unsigned flags, unsigned color)
{
  int scale = g->enhancement_scale, px, py;
  int x0 = x * scale, y0 = y * scale;
  int x1 = (x + width) * scale - 1, y1 = (y + height) * scale - 1;
  if (!width || !height || !scaled_clip(g, &x0, &y0, &x1, &y1))
    return;
  flush_render_block_buffer(g);
  for (py = y0; py <= y1; py++) {
    if (!scaled_field_visible(g, py)) continue;
    for (px = x0; px <= x1; px++)
      scaled_pixel(g, px, py, flags, color & 255, (color >> 8) & 255,
        (color >> 16) & 255, u + px / scale - x, v + py / scale - y, 0);
  }
}

static void render_line_scaled(psx_gpu_struct *g, const vertex_struct *vertices,
 unsigned flags, unsigned color)
{
  int scale = g->enhancement_scale;
  int x = vertices[0].x * scale, y = vertices[0].y * scale;
  int x1 = vertices[1].x * scale, y1 = vertices[1].y * scale;
  int dx = abs(x1 - x), dy = abs(y1 - y), sx = x < x1 ? 1 : -1;
  int sy = y < y1 ? 1 : -1, error = dx - dy, steps = max(dx, dy), i = 0;
  int clip_x0 = 0, clip_y0 = 0, clip_x1 = 1024 * scale - 1;
  int clip_y1 = 512 * scale - 1;
  if (dx >= 1024 * scale || dy >= 512 * scale ||
      !scaled_clip(g, &clip_x0, &clip_y0, &clip_x1, &clip_y1))
    return;
  flush_render_block_buffer(g);
  flags &= ~RENDER_FLAGS_TEXTURE_MAP;
  for (;;) {
    int r = color & 255, green = (color >> 8) & 255, b = (color >> 16) & 255;
    if ((flags & RENDER_FLAGS_SHADE) && steps) {
      r = vertices[0].r + (vertices[1].r - vertices[0].r) * i / steps;
      green = vertices[0].g + (vertices[1].g - vertices[0].g) * i / steps;
      b = vertices[0].b + (vertices[1].b - vertices[0].b) * i / steps;
    }
    else if (flags & RENDER_FLAGS_SHADE) {
      r = vertices[0].r; green = vertices[0].g; b = vertices[0].b;
    }
    if (x >= clip_x0 && x <= clip_x1 && y >= clip_y0 && y <= clip_y1 &&
        scaled_field_visible(g, y))
      scaled_pixel(g, x, y, flags, r, green, b, 0, 0,
        !!(g->render_state_base & RENDER_STATE_DITHER));
    if (x == x1 && y == y1) break;
    {
      int twice_error = error * 2;
      if (twice_error > -dy) { error -= dy; x += sx; }
      if (twice_error < dx) { error += dx; y += sy; }
    }
    i++;
  }
}
