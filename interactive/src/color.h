#ifndef COLOR_H
#define COLOR_H

struct rgb
{
  unsigned char r;
  unsigned char g;
  unsigned char b;
};

struct hsv
{
  unsigned char h;
  unsigned char s;
  unsigned char v;
};

struct rgb HsvToRgb(struct hsv in);

#endif
