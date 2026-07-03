#!/usr/bin/env python3
"""Regenerate assets/*.png texture sidecars from the original GalaxySwarm
sources (clone https://github.com/austinio7116/GalaxySwarm to /tmp/GalaxySwarm).
One calibrated brightness/saturation pass — rerunning is idempotent."""
from PIL import Image, ImageEnhance, ImageChops
import os

A = '/tmp/GalaxySwarm/Assets'
OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'assets')

#  name: (source, brightness, saturation)
TEXMAP = {
    'mk6':        ('Models/SciFi_Fighter-MK6-diffuse.jpg', 1.9, 1.15),
    'sf_fighter': ('SF_Fighter/Textures/SF_Fighter-Albedo-Blue.tif', 1.5, 1.25),
    'trident':    ('Trident_UV_Dekol_Color.tif', 1.5, 1.2),
    'corvette':   ('Models/SF_Corvette-F3_diffuse.jpg', 1.7, 1.25),
    'ufo':        ('Models/ufo_diffuse2.png', 1.35, 1.2),
    'asteroid1':  ('Textures/prop_asteroid_01_dff.tif', 1.5, 1.1),
    'asteroid2':  ('Textures/prop_asteroid_02_dff.tif', 1.5, 1.1),
    'asteroid3':  ('Textures/prop_asteroid_03_dff.tif', 1.5, 1.1),
    'rock':       ('Models/Rock_d.jpg', 1.45, 1.1),
    'missile':    ('Models/missile.png', 1.3, 1.15),
    'rocket':     ('Models/Rocket.png', 1.25, 1.15),
    'box':        ('Models/Box_01.png', 1.2, 1.1),
    'card':       ('Models/Card_01.png', 1.2, 1.2),
    'hourglass':  ('Models/SandClock.png', 1.2, 1.2),
    'shield':     ('Models/Shield.png', 1.2, 1.15),
    'barrel':     ('Models/barrel_low_barrel_mat_AlbedoTransparency.png', 1.25, 1.15),
    'energypack': ('Models/EnergyPack03.png', 1.2, 1.2),
}

def flatten(im):
    if im.mode == 'RGB':
        return im
    rgba = im.convert('RGBA')
    bg = Image.new('RGB', im.size, (0, 0, 0))
    bg.paste(rgba, mask=rgba.split()[3])
    return bg

for name, (rel, b, s) in TEXMAP.items():
    im = flatten(Image.open(os.path.join(A, rel)))
    im = im.resize((64, 64), Image.LANCZOS)
    im = ImageEnhance.Brightness(im).enhance(b)
    im = ImageEnhance.Color(im).enhance(s)
    im.save(os.path.join(OUT, name + '.png'))
    print(name)

# luminaris: diffuse + emissive glow composite
d = flatten(Image.open(os.path.join(A, 'Models/Luminaris Diffuse.tga')))
e = flatten(Image.open(os.path.join(A, 'Models/Luminaris Emissive.tga'))).resize(d.size)
lum = ImageChops.add(d, e).resize((64, 64), Image.LANCZOS)
lum = ImageEnhance.Brightness(lum).enhance(1.5)
lum = ImageEnhance.Color(lum).enhance(1.3)
lum.save(os.path.join(OUT, 'luminaris.png'))
print('luminaris')
