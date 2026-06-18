#!/usr/bin/env python3
import colorsys
import os
import sys

# required libraries:
# pip: numpy, pyvips, tqdm, hilbert
# also install the pyvips library on your system.
import numpy as np
import pyvips
import tqdm
from hilbert import decode
from pyvips.enums import ForeignTiffCompression

NUM_SENT = 4
HILBERT_NDIMS = 2
HILBERT_NBITS = 16

# After running this script you may want to drop caches: (slab objects and pagecache)
#   echo 3 | sudo tee /proc/sys/vm/drop_caches


def write_image_data(filename, im_data):
    """
    Converts the ping data to RGB image data. The image is extremely big (2^32 pixels) so
    the pixel data must be memory-mapped to disk.
    """
    file = open(filename, "rb")
    buf_reads = 2**15
    progress = tqdm.tqdm(total=2**32 / buf_reads)
    next_addr = 0

    color_map = np.array(
        [
            tuple(
                int(c * 255)
                for c in colorsys.hsv_to_rgb(
                    (1 - (i / 256)) * 0.9, 1, min(i / 256 + 0.2, 0.8)
                )
            )
            for i in range(256)
        ],
        dtype=np.uint8,
    )

    sample_width = 8
    averages = np.zeros(
        (2**16 // sample_width, 2**16 // sample_width), dtype=np.float16
    )

    while True:
        base_addr = next_addr
        buf = np.frombuffer(file.read(buf_reads), dtype=np.uint8)
        next_addr += buf.size

        progress.update(1)
        if not buf.size:
            break

        # We can skip this buffer if there are no replies
        # Do not modify image buffer if replies is empty, but do advance the address
        replydata = buf >> 2
        if not np.any(replydata):
            continue

        # Count the number of bits and then you have the reply counts
        # Discard the metadata bits and then you have the reply bits.
        replies = np.zeros(replydata.size, dtype=np.uint8)
        for i in range(NUM_SENT):
            replies += replydata & 1
            replydata >>= 1

        # Create mask for only the replies with a positive count.
        mask = replies > 0
        replies = replies[mask]

        # The IP addresses corresponding with the buffer we read.
        # Get the indices of the true values (offsets) and add the base address.
        addrs = np.where(mask)[0] + base_addr

        # Convert to 2d Hilbert coordinates and pixel values
        hilbert_pts = decode(addrs, HILBERT_NDIMS, HILBERT_NBITS)

        # Assign the pixel values to coordinates in bulk
        np.add.at(
            averages,
            (hilbert_pts[:, 1] // sample_width, hilbert_pts[:, 0] // sample_width),
            replies * (255 / (NUM_SENT * sample_width**2)),
        )

        lum = replies * (255 // NUM_SENT * 2 / 3) + (255 // 3)
        im_data[hilbert_pts[:, 1], hilbert_pts[:, 0]] = np.column_stack((lum, lum, lum))

    file.close()
    progress.close()

    print("Filling averaged color data")
    progress = tqdm.tqdm(total=averages.shape[0])

    averages_color = color_map[averages.astype(dtype=np.uint8)]
    w = sample_width
    for y in range(averages.shape[0]):
        progress.update(1)
        for x in range(averages.shape[1]):
            x1, y1 = x * w, y * w
            x2, y2 = x1 + w, y1 + w
            im_data[y1:y2, x1:x2] = (
                im_data[y1:y2, x1:x2]
                / 255
                * np.repeat([averages_color[y, x]], w * w, axis=0).reshape(w, w, 3)
            ).astype(np.uint8)

    progress.close()

    return im_data


def pixels_fill_reserved(im_data):
    reserved_ranges = [
        (0xE0000000, 0xF0000000),
        (0xF0000000, 0xF0000000),
        (0x00000000, 0xFF000000),
        (0x0A000000, 0xFF000000),
        (0x7F000000, 0xFF000000),
        (0x64400000, 0xFFC00000),
        (0xAC100000, 0xFFF00000),
        (0xC6120000, 0xFFFE0000),
        (0xA9FE0000, 0xFFFF0000),
        (0xC0A80000, 0xFFFF0000),
        (0xC0000000, 0xFFFFFF00),
        (0xC0000200, 0xFFFFFF00),
        (0xC0586300, 0xFFFFFF00),
        (0xC6336400, 0xFFFFFF00),
        (0xCB007100, 0xFFFFFF00),
        (0xE9FC0000, 0xFFFFFF00),
        (0xFFFFFFFF, 0xFFFFFFFF),
    ]


def main():
    if len(sys.argv) < 2:
        sys.stderr.write(f"usage: {__file__} <dat>\n")
        exit(1)

    # delete file
    with open("image.dat", "wb") as f:
        f.write(b"")

    # map that data to an numpy array
    arr = np.memmap("image.dat", mode="w+", shape=(2**16, 2**16, 3))
    write_image_data(sys.argv[1], arr)
    arr.flush()

    # Write image to disk
    print("Converting image to TIFF")
    image = pyvips.Image.new_from_array(arr, interpretation="rgb")

    # image.write_to_file("output.tif")
    image.tiffsave(
        "bin/echo_map.tiff",
        compression=ForeignTiffCompression.DEFLATE,
        tile=True,
        pyramid=True,
        bigtiff=True,
    )

    os.remove("image.dat")

    print("Done. Deleted cached file image.dat")


if __name__ == "__main__":
    main()
