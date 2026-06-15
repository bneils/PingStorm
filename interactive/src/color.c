#include "color.h"

// Source - https://stackoverflow.com/a/14733008
// Posted by Leszek Szary, modified by community. See post 'Timeline' for change history
// Retrieved 2026-06-14, License - CC BY-SA 4.0

struct rgb HsvToRgb(struct hsv in)
{
  struct rgb out;
  unsigned char region, remainder, p, q, t;

  if (in.s == 0) {
    out.r = in.v;
    out.g = in.v;
    out.b = in.v;
    return out;
  }

  region = in.h / 43;
  remainder = (in.h - (region * 43)) * 6;

  p = (in.v * (255 - in.s)) >> 8;
  q = (in.v * (255 - ((in.s * remainder) >> 8))) >> 8;
  t = (in.v * (255 - ((in.s * (255 - remainder)) >> 8))) >> 8;

  switch (region) {
  case 0:
    out.r = in.v; out.g = t; out.b = p;
    break;
  case 1:
    out.r = q; out.g = in.v; out.b = p;
    break;
  case 2:
    out.r = p; out.g = in.v; out.b = t;
    break;
  case 3:
    out.r = p; out.g = q; out.b = in.v;
    break;
  case 4:
    out.r = t; out.g = p; out.b = in.v;
    break;
  default:
    out.r = in.v; out.g = p; out.b = q;
    break;
  }

  return out;
}
