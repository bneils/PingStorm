#include <SDL3/SDL.h>
#include <SDL3/SDL_events.h>
#include <SDL3/SDL_init.h>
#include <SDL3/SDL_oldnames.h>
#include <SDL3/SDL_pixels.h>
#include <SDL3/SDL_rect.h>
#include <SDL3/SDL_surface.h>
#include <stdint.h>
#include "../../src/macros.h"
#include "hilbert.h"
#include "lod.h"
#include "color.h"

#define WINDOW_TITLE "Internet Browser"
#define SCREEN_WIDTH 1024

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
lod_render (FILE *lod, SDL_Surface *surface, int ax, int ay, int aw, int ah)
{
  if (!surface->w)
    return;

  // How many addresses are supposed to be in a pixel, width-wise
  uint64_t density = aw / surface->w;

  // We need to pick a level of detail
  size_t lod_base = 0;            // where the lod starts in the file.
  size_t lod_size = (1UL << 32);  // size of the lod level.
  size_t lod_width = (1 << 16);   // how many entries wide the lod level is.

  // We take the order of the most significant bit and use that as the LOD level.
  // The variable being shifted here _must_ be unsigned.
  int scale = 1;
  while (density >>= 1) {
    lod_base += lod_size;
    lod_size /= 4;
    lod_width /= 2;
    scale *= 2;
  }

  SDL_Rect rect = {0, 0, surface->w, surface->h};
  const SDL_PixelFormatDetails *format_details = SDL_GetPixelFormatDetails(surface->format);
  SDL_FillSurfaceRect(surface, &rect, SDL_MapRGB(format_details, NULL, 0, 100, 0));

  // Determine the width of each LOD.
  double cells_draw_width = (double)aw / scale;
  double cell_draw_width = (double)surface->w / cells_draw_width;

  // Determine the starting screen position (zero or negative)
  // of the LOD bits.

  ay /= scale;
  ax /= scale;
  aw /= scale;
  ah /= scale;

  // Initialize LOD index to the top-left of the viewed region
  size_t lod_idx = lod_base + ay * lod_width + ax;

  wlog (LEVEL_DEBUG, "cell width: %lf", cell_draw_width);
  wlog (LEVEL_DEBUG, "cells width: %lf", cells_draw_width);
  wlog (LEVEL_DEBUG, "cell height: %d, cell width: %d", ah, aw);
  wlog (LEVEL_DEBUG, "scale: %d",scale);

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
    color_table[i] = SDL_MapRGB(format_details, NULL, out.r, out.g, out.b);
  }

  for (int rel_y = 0; rel_y < ah; ++rel_y) {
    // Jump to LOD file address of first pixel in the row
    fseek (lod, lod_idx, SEEK_SET);

    for (int rel_x = 0; rel_x < aw; ++rel_x) {
      // Read the corresponding LOD value and convert it to a color.
      unsigned char val;
      ASSERT (fread (&val, sizeof (val), 1, lod));
      uint32_t color = color_table[val];

      // Fill the LOD where it sits on the screen
      SDL_Rect rect;
      rect.y = cell_draw_width * rel_y;
      rect.x = cell_draw_width * rel_x;
      rect.w = cell_draw_width < 1 ? 1 : cell_draw_width;
      rect.h = cell_draw_width < 1 ? 1 : cell_draw_width;
      SDL_FillSurfaceRect(surface, &rect, color);
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

  int w = 1 << 12, x = 0, y = 0;

  for (;;) {
    SDL_Event e;
    int dx = w > 10 ? w / 10 : 1;

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
            if (w > 2)
              w /= 2;
            break;
          case SDL_SCANCODE_Q:
            if (w < (1 << 17))
              w *= 2;
            break;
          default:
            break;
          }
          break;
  		}
  	}

    lod_render (lod_file, surface, x, y, w, w);
    ASSERT (SDL_UpdateWindowSurface (window));
  	SDL_Delay(20);
  }
}
