#include <SDL3/SDL.h>
#include <SDL3/SDL_events.h>
#include <SDL3/SDL_init.h>
#include <SDL3/SDL_oldnames.h>
#include <SDL3/SDL_pixels.h>
#include <SDL3/SDL_rect.h>
#include <SDL3/SDL_surface.h>
#include <bits/time.h>
#include <stdint.h>
#include <math.h>
#include <time.h>
#include "../../src/macros.h"
#include "hilbert.h"
#include "lod.h"
#include "color.h"

#define WINDOW_TITLE "Internet Browser"
#define SCREEN_WIDTH 750

/*
 * Use a quad tree to select what pixels to give for a certain resolution.
 * This quad tree can be used to partially select pixels from the file.
 * The viewport updates every second or so and rewrites the frame buffer.
 * WASD moves the viewport.
 * Q expands the viewport (zooms out).
 * E shrinks the viewport (zooms in).
 *
 * The amount of additional memory to store the quad tree can be calculated
 * from this equation:
 * 4^0 + 4^1 + ... + 4^(log_4(n))
 * It can be represented as the number "111...111" in base 4 with log_4(n) 1s.
 * If you take 4^(log_4(n)+1)-1, you get
 * "333...333" in base 4 with log_4(n) 3s.
 * Then divide by 3 to get the amount of memory required to build the quad tree.
 * This is (4n-1)/3 or roughly equates to 4/3 the memory required from 4GB.
 *
 * Obviously we do not load the lowest level in memory, since that is enormous and never needed all at once.
 * This means we only need to store 1/3 of the data, which is still 4/3 GB.
 * To reduce this further we can remove another level, which provides 1/4 of the cost savings.
 * After this, we are only storing 1/3 GB = 333 MB.
 * We can do this further to store only (1/3-1/4)=1/12 GB = 83MB.
 *
 * We could store the entire tree on disk to simplify retrievals.
 *
 * Retrieving the displayed pixels for a fixed viewport would be to first identify the largest identifiable
 * quad size based on window resolution and zoom factor. Then we would recurse to that level in that tree,
 * and ensure nodes in each level are stored consecutively and all together, so to reduce OS cache misses, disk reads,
 * and allow the OS to manage buffer and cache use.
 *
 * It is possible to calculate the beginning of each quad level in the flattened tree and then use that as a base
 * for calculating the index of an X,Y in each level.
 */

unsigned
int_log (unsigned x)
{
  unsigned n = 0;
  while (x >>= 1)
    n++;
  return n;
}

void
lod_render (FILE *lod, SDL_Renderer *renderer, SDL_Surface *surface, int64_t real_x, int64_t real_y, int64_t real_w, int64_t real_h)
{
  ASSERT (real_h > 0 && real_w > 0);

  // Fill background
  SDL_Rect rect = {0, 0, surface->w, surface->h};
  SDL_FillSurfaceRect (surface, &rect, SDL_MapSurfaceRGB (surface, 0, 0, 0));

  // We need to pick a level of detail
  int64_t lod_base = 0;            // where the lod starts in the file.
  int64_t lod_size = (1UL << 32);  // size of the lod level.
  int64_t lod_width = (1 << 16);   // how many entries wide the lod level is.

  // We take the order of the most significant bit and use that as the LOD level.
  // The variable being shifted here _must_ be unsigned.
  // How many addresses are supposed to be in a pixel, width-wise
  uint32_t scale = 1;
  int lod_levels = round (log2 ((double) real_w / surface->w));
  if (lod_levels > 15)
    lod_levels = 15;
  for (int i = 0; i < lod_levels; ++i) {
    lod_base += lod_size;
    lod_size /= 4;
    lod_width /= 2;
    scale *= 2;
  }

  // Determine the width of each LOD.
  double cell_draw_width = ((double)surface->w / real_w) * scale;

  //wlog (LEVEL_DEBUG, "cdw: %lf", cell_draw_width);

  // The LOD map has smaller dimensions than our real coordinates,
  // so we should map ours down. Note: these values may be negative.
  // A negative value symbols that the LOD starts at a positive offset
  // from the top left.
  int64_t lod_x1 = real_x / scale;
  int64_t lod_y1 = real_y / scale;
  int64_t lod_x2 = lod_x1 + real_w / scale;
  int64_t lod_y2 = lod_y1 + real_h / scale;

  int64_t scr_y_off = -lod_y1 * cell_draw_width;
  int64_t scr_x_off = -lod_x1 * cell_draw_width;

  lod_x1 = MAX (lod_x1, 0);
  lod_y1 = MAX (lod_y1, 0);
  lod_x1 = MIN (lod_x1, lod_width);
  lod_y1 = MIN (lod_y1, lod_width);

  lod_x2 = MAX (lod_x2, 0);
  lod_y2 = MAX (lod_y2, 0);
  lod_x2 = MIN (lod_x2, lod_width);
  lod_y2 = MIN (lod_y2, lod_width);

  // Get the starting index
  int64_t lod_idx = lod_base + lod_y1 * lod_width + lod_x1;

  // Build the color table
  uint32_t color_table[256];
  for (int i = 0; i < 256; ++i) {
    unsigned lum = i;
    int logx = int_log (i);
    lum = (255 * logx * 3 / 5 + i * logx * 2 / 5) / 8;
    ASSERT (lum <= 255);

    struct hsv color = {
      .h = (255 - i) * 2 / 3,
      .s = 255,
      .v = lum,
    };

    struct rgb out = HsvToRgb(color);
    color_table[i] = SDL_MapSurfaceRGB(surface, out.r, out.g, out.b);
  }

  int64_t scr_wid = ceil (cell_draw_width < 0.001 ? 1 : cell_draw_width);
  SDL_Rect scr_rect = {
    .x = 0,
    .y = 0,
    .w = scr_wid,
    .h = scr_wid,
  };

  const SDL_PixelFormatDetails *details = SDL_GetPixelFormatDetails (surface->format);

  // Begin drawing
  for (int64_t lod_yit = lod_y1; lod_yit < lod_y2; ++lod_yit) {
    // Jump to LOD file address of first pixel in the row
    fseek (lod, lod_idx, SEEK_SET);

    for (int64_t lod_xit = lod_x1; lod_xit < lod_x2; ++lod_xit) {
      // Read the corresponding LOD value and convert it to a color.
      unsigned char val;
      if (!fread (&val, sizeof (val), 1, lod))
        return;
      // Fill the LOD where it sits on the screen
      scr_rect.x = cell_draw_width * lod_xit + scr_x_off;
      scr_rect.y = cell_draw_width * lod_yit + scr_y_off;
      if (cell_draw_width > 1) {
        SDL_FillSurfaceRect (surface, &scr_rect, color_table[val]);
      } else if (scr_rect.y >= 0 && scr_rect.y < surface->h && scr_rect.x >= 0 && scr_rect.x < surface->w &&
        scr_rect.y + scr_rect.h < surface->h && scr_rect.x + scr_rect.w < surface->w) {
        uint64_t idx = details->bytes_per_pixel * (surface->w * scr_rect.y + scr_rect.x);
        //wlog (LEVEL_DEBUG, "Accessing raw buffer");
        *(uint32_t *)(surface->pixels + idx) = color_table[val];
      }
    }
    lod_idx += lod_width;
  }
}

int
main (void)
{
  // Load the LOD data.
  FILE *lod_file = fopen ("lod.dat", "r");
  if (!lod_file) {
    wlog (LEVEL_INFO, "Creating LOD file. I would walk away for 10 minutes!");
    lod_file = ASSERT_NOT (lod_create ("lod.dat", "../ping.dat"), NULL);
  }

  ASSERT (SDL_InitSubSystem (SDL_INIT_VIDEO));
  SDL_Window *window = ASSERT_NOT (SDL_CreateWindow (WINDOW_TITLE, SCREEN_WIDTH, SCREEN_WIDTH, 0), NULL);
  SDL_Surface *surface = ASSERT_NOT (SDL_GetWindowSurface (window), NULL);
  SDL_Renderer *renderer = ASSERT_NOT (SDL_GetRenderer (window), NULL);

  float w = 1 << 16, x = 0, y = 0;

  for (;;) {
    struct timespec start, end;
    clock_gettime (CLOCK_MONOTONIC_RAW, &start);

    SDL_Event e;
    int dx = ceil (w / 10);

  	while (SDL_PollEvent(&e)) {
  		switch (e.type) {
  			case SDL_EVENT_QUIT:
  				exit (0);
  				break;
  			case SDL_EVENT_MOUSE_BUTTON_DOWN:
  				break;
        case SDL_EVENT_KEY_DOWN:
          switch (e.key.scancode) {
          case SDL_SCANCODE_W:
            y -= dx;
            break;
          case SDL_SCANCODE_A:
            x -= dx;
            break;
          case SDL_SCANCODE_S:
            y += dx;
            break;
          case SDL_SCANCODE_D:
            x += dx;
            break;
          case SDL_SCANCODE_E:
            if (w > 1)
              w /= 1.5;
            break;
          case SDL_SCANCODE_Q:
            if (w < (1 << 20))
              w *= 1.5;
            break;
          default:
            break;
          }
          break;
  		}
  	}

    if (x < -(1 << 15)) x = -(1 << 15);
    else if (x > (1 << 17)) x = 1 << 17;
    if (y < -(1 << 15)) y = -(1 << 15);
    else if (y > (1 << 17)) y = 1 << 17;


    lod_render (lod_file, renderer, surface, x - w / 2, y - w / 2, w, w);

    clock_gettime (CLOCK_MONOTONIC_RAW, &end);
    long diffmsec = (end.tv_nsec - start.tv_nsec) / 1e6;
    if (end.tv_sec > start.tv_sec)
      diffmsec += 1e3;

    wlog (LEVEL_DEBUG, "%ld fps\n", 1000 / (diffmsec+1));

    ASSERT (SDL_UpdateWindowSurface (window));

    long delay = 20 - diffmsec;
    if (delay > 0)
     	SDL_Delay(delay);
  }
}
