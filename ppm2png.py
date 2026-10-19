#!/usr/bin/env python3
"""ppm2png.py <in.ppm>... : convert host_sim screenshots to PNG (3x nearest-neighbour)."""
import sys
from PIL import Image
for p in sys.argv[1:]:
    Image.open(p).resize((720, 480), Image.NEAREST).save(p.rsplit('.', 1)[0] + '.png')
