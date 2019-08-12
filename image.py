#!/usr/bin/env python3

import sys
from math import sin, cos, sqrt

def flatten(l):
    return [item for sublist in l for item in sublist]

# E-Ink display takes 16 colour values 0x0f, 0x1f, 0x2f, ... 0xff.
def pixels_to_bytes(pixels):
    return bytes(map(lambda x: (x << 4) | 0xf, flatten(pixels)))

hardcode = [
    [ 0, 3, 6, 9, ],
    [ 2, 5, 8, 11, ],
    [ 4, 6, 9, 12, ],
    [ 6, 7, 10, 13, ],
]

cursor = [[0xf for x in range(32)] for y in range(48)]

for y in range(48):
    for x in range(32):
        # left and diag
        if x == 0 and y < 46 or x == y and y < 32:
            cursor[y][x] = 0
        # left under and right under
        elif x == 46-y and x < 11 or y == 31 and x >= 18:
            cursor[y][x] = 0
        # left tail
        elif (x - 11) == (y - 36) // 2 and x >= 11:
            cursor[y][x] = 0
        # right tail
        elif (x - 18) == (y - 31) // 2 and x >= 18 and y < 45:
            cursor[y][x] = 0
        # bottom tip
        elif (x - 21) // 2 == -(y - 45) and y > 43:
            cursor[y][x] = 0


funky = [[int((x-y + 2 * x*y)/30)&0xf for x in range(245)] for y in range(245)]
blobs = [[int(sin((x*x + y*y)/8)*8 + 8)&0xf for x in range(245)] for y in range(245)]

def draw(image, x, y):
    h = len(image)
    w = len(image[0])
    print(f"xfer 0 {w} {h}")
    sys.stdout.flush()
    sys.stdout.buffer.write(pixels_to_bytes(image))
    print(f"blit 0 {x} {y} {w} {h}")

draw(funky, 0, 0)
draw(blobs, 245, 0)

draw(cursor, 920, 10)

