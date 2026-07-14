#!/usr/bin/env python3
import colorsys
import sys
import uuid

import numpy as np
import tqdm
from hilbert import decode, encode
from PIL import Image

"""
Uses same 12th order Hilbert curve as:
https://www.caida.org/archive/id-consumption/census-map/images/20061108.png
https://xkcd.com/195/
"""


### https://gist.github.com/legaultmarc/92c4065e157eb93d57a5
def vector_map(data, _map):
    """Remaps numeric values in a data vector.
    :param data: A numpy array of integer or float dtype.
    :param _map: A list of tuple representing the mappings.
                 Alternatively, a dict can be provided directly.
                 ``[(2, 1), (1, 0), (3, -10)]`` would transform all the
                 ``2 -> 1``, ``1 -> 0`` and ``3 -> -10``.
    :returns: A remapped numpy array.
    The strategy used to avoid collisions when sequentially remapping is to
    use transitive mapping. This means that the mapping is done in two steps:
    A mapping to a unique (random) value and then a subsequent mapping to the
    target value.
    This strategy is only used if there are collisions.
    >>> import numpy as np
    >>> from remap import vector_map
    >>> a = np.array([-9, 2, 2, 1, 2, 1, -9, 2])
    >>> vector_map(a, [(-9, np.nan), (2, 1.0), (1, 0.0)])
    array([ nan,   1.,   1.,   0.,   1.,   0.,  nan,   1.])
    Type consistency is annoying, but it is safer this way as there is no risk
    of weird comparisons or type mistakes.
    """

    if type(_map) is not dict:
        _map = dict(_map)

    # Check the dtype of the vector and the map.
    if np.issubdtype(data.dtype, np.uint8):
        source_dtype = int
    elif np.issubdtype(data.dtype, float):
        source_dtype = float
    else:
        raise TypeError(
            "Invalid dtype: '{}'. This function only allows "
            "int or float vectors.".format(data.dtype)
        )

    keys = set(_map.keys())
    targets = set(_map.values())

    # Infer the target dtype.
    target_dtype = set([float if np.isnan(t) else type(t) for t in targets])

    if len(target_dtype) != 1:
        raise TypeError(
            "Ambiguous target dtype. Make sure that the provided "
            "mapper uses consistent type for the second element "
            "of the tuples."
        )
    target_dtype = target_dtype.pop()

    if target_dtype is int and source_dtype is float:
        raise TypeError(
            "Remapping floats to integers is not possible "
            "because of lost of data in type cast (this can "
            "be fixed by using floats as the target values)."
        )

    out = data.astype(target_dtype)

    for key, target in _map.items():
        if np.isnan(key):
            raise TypeError("Can't use NaNs as mapping keys.")

        if target in keys:
            # There will be a collision, so we need to use the transitive
            # mapping.
            transitive_key = hash(str(uuid.uuid4()))
            out[data == key] = transitive_key
            out[out == transitive_key] = target
        else:
            out[data == key] = target

    return out


### END SOURCE


def extract_reply_data(filename, width, num_sent):
    buf_reads = 2**14

    with (
        open(filename, "rb") as file,
        tqdm.tqdm(total=2**32 / buf_reads) as progress,
    ):
        base_addr = 0
        addrs_per_px_w = int(2**16 / width)
        im_replies = np.zeros((width, width))

        count_bits = {i: bin(i)[2:].count("1") for i in range(2**num_sent)}

        while True:
            buf = np.fromiter(file.read(buf_reads), dtype=np.uint8)
            progress.update(1)
            if not buf.size:
                break
            # Check buffer for any replies
            # We can skip this buffer if there are no replies
            replies = vector_map(buf >> 2, count_bits)
            if not np.any(replies > 0):
                base_addr += buf.size
                continue

            # The ip addresses we need to convert
            # This code is quite slow, will be unbearable later on
            addrs = np.arange(base_addr, base_addr + buf.size)

            zipped = np.dstack([addrs, replies])[0]
            filtered = np.where(zipped[:, 1] > 0)[0]

            # Type checker complains without this
            points: np.ndarray[tuple[int]] = decode(zipped[filtered][:, 0], 2, 16) // addrs_per_px_w

            # Should be refactored
            for xy, r in zip(points, zipped[filtered][:, 1]):
                x, y = xy
                im_replies[y, x] += r
                assert r <= num_sent, "r cannot exceed num_sent"
            base_addr += buf.size
    return im_replies


def image_reply_heatmap(im_replies, width, num_sent):
    with tqdm.tqdm(total=width) as progress:
        pixels = np.zeros((width, width, 3), dtype=np.uint8)
        addrs_per_px = int(2**32 / width / width)
        color_map = [
            tuple(
                int(c * 255)
                for c in colorsys.hsv_to_rgb(i / 256 * 2 / 3, 1, min(1.1 - i / 256, 1))
            )
            for i in range(256, 0, -1)
        ]

        for y in range(width):
            progress.update(1)
            for x in range(width):
                # is at most addrx_per_px * num_sent.
                avg_replies = im_replies[y, x] / addrs_per_px / num_sent
                assert avg_replies <= 1, (
                    "The amount of replies in this pixel far exceeds what is normal"
                )
                pixels[y, x] = color_map[int(min(avg_replies, 1) * 255)]
        return pixels


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

    width = 2**13

    print("Extracting image data")
    im_replies = extract_reply_data(sys.argv[1], width, num_sent=4)
    print("Creating image")
    pixels = image_reply_heatmap(im_replies, width, num_sent=4)
    print("Filling reserved regions")
    pixels = pixels_fill_reserved(pixels, width)

    # Write image to disk
    im = Image.fromarray(pixels, mode="RGB")
    im.save("bin/pings.png")


if __name__ == "__main__":
    main()
