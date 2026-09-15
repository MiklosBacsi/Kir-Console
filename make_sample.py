#!/usr/bin/env python3
"""Draw a filled star silhouette PNG (transparent background) for ./object."""

from PIL import Image, ImageDraw
import math

SIZE = 512
img = Image.new("RGBA", (SIZE, SIZE), (0, 0, 0, 0))
draw = ImageDraw.Draw(img)

cx = cy = SIZE / 2
outer = SIZE * 0.42
inner = SIZE * 0.17
points = []
for i in range(10):
    r = outer if i % 2 == 0 else inner
    a = -math.pi / 2 + i * math.pi / 5
    points.append((cx + r * math.cos(a), cy + r * math.sin(a)))

draw.polygon(points, fill=(255, 255, 255, 255))
img.save("sample.png")
print("wrote sample.png")
