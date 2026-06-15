#include <stdio.h>
#define _FILE_OFFSET_BITS 64

#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <stdlib.h>

#include "lod.h"
#include "hilbert.h"
#include "../../src/macros.h"

#define IPV4_SIZE (1UL << 32)
#define LOD_SIZE ((4 * IPV4_SIZE - 1) / 3)

/* Reads data from the ping path and writes it to the LOD file (1st parameter).
 * Returns a pointer to a file on success and NULL on an error.
 */
FILE *
lod_create (const char *lod_path, const char *ping_path)
{
  FILE *lod_file, *ping_file;
  unsigned char *lod;

  lod_file = NULL;
  ping_file = NULL;
  lod = NULL;

  lod_file = fopen (lod_path, "w+");
  if (!lod_file)
    goto cleanup;

  ping_file = fopen (ping_path, "r");
  if (!ping_file)
    goto cleanup;

  // Stretch the file
  if (fseek (lod_file, LOD_SIZE - 1, SEEK_SET) == -1)
    goto cleanup;
  fwrite ("", 1, 1, lod_file);
  if (ferror (lod_file))
    goto cleanup;

  // Map the file for non-sequential reads and writes
  rewind (lod_file);

  lod = mmap (NULL, LOD_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fileno (lod_file), 0);
  if (lod == MAP_FAILED)
    goto cleanup;

  bitmask_t coords[2] = {0, 0};

  wlog (LEVEL_TRACE, "Converting first LOD to Hilbert-space");

  // Read first layer from ping file and convert to integer counts.
  for (size_t addr = 0, nread = 0;; addr += nread) {
    unsigned char buf[4096];

    if ((addr / sizeof buf) % 5000 == 0)
      wlog (LEVEL_TRACE, "Address: %llu", addr);

    nread = fread (buf, 1, sizeof (buf), ping_file);
    // reached the end of the file
    if (!nread && feof (ping_file))
      break;

    // handle eof or error
    if (nread < sizeof (buf) && ferror (ping_file))
      goto cleanup;

    // Convert the data
    for (size_t i = 0; i < nread; ++i) {
      // Discard the lower 2 metadata bits
      unsigned int rdata = buf[i] >> 2;
      size_t lod_idx;
      // Get index with hilbert curve
      lod_idx = (coords[0] << 16) + coords[1];
      // Count the number of bits quickly with fewest # of memory accesses
      lod[lod_idx] =
        ((rdata >> 5) + (rdata >> 4 & 1) + (rdata >> 3 & 1) + \
        (rdata >> 2 & 1) + (rdata >> 1 & 1) + (rdata & 1)) * 255 / 6;
      hilbert_incr (2, 16, coords);
    }
  }

  // Create the next layers of the LOD file
  for (size_t len = 1 << 16, start = 0, dst = IPV4_SIZE; len >= 2; len /= 2) {
    wlog (LEVEL_TRACE, "Starting LOD with size (%d)^2, start=%llu", len, start);
    // Jump two rows at a time in the previous layer.
    size_t end = dst;
    for (size_t i = start; i < end; i += len * 2)
      // Iterate the first of two rows, by 2
      for (size_t j = i; j < i + len; j += 2) {
        unsigned int avg = (lod[j] + lod[j + 1] + lod[j + len] + lod[j + len + 1]) / 4;
        lod[dst++] = avg;
      }
    start = end;
  }

  // Write buffered changes and close resources
  msync (lod, LOD_SIZE, MS_SYNC);
  munmap (lod, LOD_SIZE);
  fclose (ping_file);
  return lod_file;
cleanup:
  fclose (ping_file);
  munmap (lod, LOD_SIZE);
  fclose (lod_file);
  return NULL;
}
