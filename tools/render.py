import sys

if len(sys.argv) < 4:
    print("""
USAGE: render filename width height
       width x height should equal the size of the file""")
    sys.exit(1)

import pygame

width = int(sys.argv[2])
height = int(sys.argv[3])

# basic constants to set up your game
WIDTH = 2800
HEIGHT = 768
FPS = 5
BLACK = (0, 0, 0,)
GREEN = (0, 0xFF, 0,)

# initialize pygame
pygame.init()
screen = pygame.display.set_mode((WIDTH, HEIGHT))
pygame.display.set_caption("My Game")
clock = pygame.time.Clock()

data = []
with open(sys.argv[1], 'rb') as f:
    data = list(f.read().strip())

pixels = []
for p in data:
    pixels.append(p >> 4)

print(len(pixels))

running = True
while running:
    clock.tick(FPS)
    for event in pygame.event.get():
        if event.type == pygame.QUIT:
            pygame.quit()
        # add any other events here (keys, mouse, etc.)

    # Game loop part 2: Updates #####

    # Game loop part 3: Draw #####
    screen.fill(GREEN)
    canvas = pygame.PixelArray(screen)

    for i in range(len(pixels) // width):
        for j in range(width):
            if j + i * width >= len(pixels):
                break
            v = pixels[j + i * width] * 0x111111
            canvas[j * 2 + 0, i * 2 + 0] = v
            canvas[j * 2 + 1, i * 2 + 0] = v
            canvas[j * 2 + 0, i * 2 + 1] = v
            canvas[j * 2 + 1, i * 2 + 1] = v
        canvas[1012*4//5, i] = 0x6622ff
    canvas.close()



    
    # after drawing, flip the display
    pygame.display.flip()

# close the window
pygame.quit()


