#!/usr/bin/env python3
"""Writes a small Lottie animation (Bodymovin JSON) for the animated-layers
demo: a magenta star that spins and pulses inside a cyan ring, 2 s at 30 fps,
512x512. Generated at runtime; no media in the repo.
    make_lottie.py OUT.json
"""
import json, sys

W = H = 512
FR, OP = 30, 60

def kf(t, v, ease=True):
    k = {'t': t, 's': v if isinstance(v, list) else [v]}
    if ease:
        k['i'] = {'x': [0.4], 'y': [1]}
        k['o'] = {'x': [0.6], 'y': [0]}
    return k

def transform(pos=(W / 2, H / 2), rot=None, scale=None):
    return {'a': {'a': 0, 'k': [0, 0, 0]},
            'p': {'a': 0, 'k': [pos[0], pos[1], 0]},
            's': scale or {'a': 0, 'k': [100, 100, 100]},
            'r': rot or {'a': 0, 'k': 0},
            'o': {'a': 0, 'k': 100}}

def layer(ind, name, shapes, ks):
    return {'ddd': 0, 'ind': ind, 'ty': 4, 'nm': name, 'sr': 1, 'ks': ks, 'ao': 0,
            'shapes': shapes, 'ip': 0, 'op': OP, 'st': 0, 'bm': 0}

star = [{'ty': 'gr', 'nm': 'star', 'it': [
    {'ty': 'sr', 'sy': 1, 'd': 1, 'pt': {'a': 0, 'k': 5}, 'p': {'a': 0, 'k': [0, 0]},
     'r': {'a': 0, 'k': 0}, 'ir': {'a': 0, 'k': 70}, 'is': {'a': 0, 'k': 0},
     'or': {'a': 0, 'k': 160}, 'os': {'a': 0, 'k': 0}, 'nm': 'star path'},
    {'ty': 'fl', 'c': {'a': 0, 'k': [1, 0.169, 0.839, 1]}, 'o': {'a': 0, 'k': 100}, 'r': 1, 'nm': 'magenta'},
    {'ty': 'tr', 'p': {'a': 0, 'k': [0, 0]}, 'a': {'a': 0, 'k': [0, 0]}, 's': {'a': 0, 'k': [100, 100]},
     'r': {'a': 0, 'k': 0}, 'o': {'a': 0, 'k': 100}}]}]
ring = [{'ty': 'gr', 'nm': 'ring', 'it': [
    {'ty': 'el', 'd': 1, 's': {'a': 0, 'k': [440, 440]}, 'p': {'a': 0, 'k': [0, 0]}, 'nm': 'circle'},
    {'ty': 'st', 'c': {'a': 0, 'k': [0.137, 0.867, 0.949, 1]}, 'o': {'a': 0, 'k': 100}, 'w': {'a': 0, 'k': 18},
     'lc': 2, 'lj': 2, 'nm': 'cyan'},
    {'ty': 'tr', 'p': {'a': 0, 'k': [0, 0]}, 'a': {'a': 0, 'k': [0, 0]}, 's': {'a': 0, 'k': [100, 100]},
     'r': {'a': 0, 'k': 0}, 'o': {'a': 0, 'k': 100}}]}]

spin = {'a': 1, 'k': [kf(0, 0), kf(OP, 360, ease=False)]}
pulse = {'a': 1, 'k': [kf(0, [80, 80, 100]), kf(OP / 2, [112, 112, 100]), kf(OP, [80, 80, 100])]}
ringpulse = {'a': 1, 'k': [kf(0, [100, 100, 100]), kf(OP / 2, [92, 92, 100]), kf(OP, [100, 100, 100])]}

doc = {'v': '5.7.4', 'fr': FR, 'ip': 0, 'op': OP, 'w': W, 'h': H, 'nm': 'Unicorn star', 'ddd': 0, 'assets': [],
       'layers': [layer(1, 'star', star, transform(rot=spin, scale=pulse)),
                  layer(2, 'ring', ring, transform(scale=ringpulse))]}
json.dump(doc, open(sys.argv[1], 'w'))
