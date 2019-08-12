#!/usr/bin/env python3

import sys


f = open('/dev/eink0', 'wb')


def draw(image, x, y, w, h):
    f.write(f"xfer 0 {w} {h}\n".encode('utf8'))
    f.write(image)
    f.write(f"blit 0 {x} {y} {w} {h}\n".encode('utf8'))

# FIXME the last bit is less than 32 pixels high
for Y in range(0, 1080, 32):
    chunk = []
    for y in range(Y, Y+32):
        oy = (y / 540) - 1
        for x in range(1920):
            ox = (x / 640) - 2.2
            c = 0
    
            x2 = y2 = 0
            x = 0
            y = 0
            while c < 63 and x2 + y2 < 4:
                y2 = y * y
                y = 2 * x * y + oy
                x2 = x * x
                x = x2 - y2 + ox
                c += 1
            chunk.append(c << 2)
    draw(bytes(chunk), 0, Y, 1920, 32)

