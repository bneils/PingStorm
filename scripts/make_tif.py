#!/usr/bin/env python3
import colorsys
import sys

# required libraries:
# pip: numpy, pyvips, tqdm, hilbert
# also install the pyvips library on your system.
import numpy as np
import pyvips
import tqdm
from hilbert import decode, encode
from pyvips.enums import ForeignTiffCompression

NUM_SENT = 4
HILBERT_NDIMS = 2
HILBERT_NBITS = 16


def write_image_data(filename, im_data):
    """
    Converts the ping data to RGB image data. The image is extremely big (2^32 pixels) so
    the pixel data must be memory-mapped to disk.
    """
    file = open(filename, "rb")
    buf_reads = 2**14
    progress = tqdm.tqdm(total=2**32 / buf_reads)
    base_addr = 0

    color_map = np.array(
        [
            tuple(
                int(c * 255)
                for c in colorsys.hsv_to_rgb(
                    (1 - (i / 256)) * 0.9, 1, min(i / 256 + 0.2, 0.8)
                )
            )
            for i in range(256)
        ]
    )

    while True:
        buf = np.fromiter(file.read(buf_reads), dtype=np.uint8)
        progress.update(1)
        if not buf.size:
            break

        # Convert the sequenced reply data in each byte to an integer number of replies.
        # Discard the metadata bits and then shift in a loop, summing the bits
        replydata = buf >> 2
        replies = np.zeros(replydata.size)
        for i in range(NUM_SENT):
            replies += replydata & 1
            replydata >>= 1

        # We can skip this buffer if there are no replies
        # Do not modify image buffer if replies is empty, but do advance the address
        if not np.any(replies > 0):
            base_addr += buf.size
            continue

        # The IP addresses corresponding with the buffer we read.
        addrs = np.arange(base_addr, base_addr + buf.size)

        # Only iterate over the replied addresses
        pairs = np.dstack((addrs, replies))[0]
        replied_pairs = pairs[pairs[:, 1] > 0]

        # Convert to 2d Hilbert coordinates and pixel values
        hilbert_pts = decode(replied_pairs[:, 0], HILBERT_NDIMS, HILBERT_NBITS)
        pixel_values = replied_pairs[:, 1].astype(np.uint8, copy=False) * (
            255 // NUM_SENT
        )
        pixel_values = color_map[pixel_values]
        # Assign the pixel values to coordinates in bulk
        im_data[hilbert_pts[:, 1], hilbert_pts[:, 0]] = pixel_values

        base_addr += buf.size

    file.close()
    progress.close()

    return im_data


def pixels_fill_reserved(pixels, width):
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

    # Map the image's coordinates to IP address space.
    # Test each IP for a reserved range, coloring it appropriately
    addrs_per_px_w = int(2**16 / width)
    with tqdm.tqdm(total=width) as progress:
        for y in range(width):
            progress.update(1)
            px_addrs = encode(
                np.array(
                    [(x * addrs_per_px_w, y * addrs_per_px_w) for x in range(width)]
                ),
                2,
                16,
            )
            for i, addr in enumerate(px_addrs):
                for network, netmask in reserved_ranges:
                    if (addr & netmask) == network:
                        x = i % width
                        pixels[y, x] = (158, 89, 255)
                        break
    return pixels


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

    # arr = np.memmap("image.dat", mode="r", shape=(2**16, 2**16, 3))

    # Write image to disk
    print("Converting image to TIFF")
    image = pyvips.Image.new_from_array(arr, interpretation="rgb")

    # image.write_to_file("output.tif")
    image.tiffsave(
        "output.tiff",
        compression=ForeignTiffCompression.DEFLATE,
        tile=True,
        pyramid=True,
        bigtiff=True,
    )

    print("Saved. You may delete image.dat now.")


if __name__ == "__main__":
    main()
