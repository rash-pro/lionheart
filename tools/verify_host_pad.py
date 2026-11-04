"""Synthetic controller schedules and guest movement assertion helpers."""
import argparse
from concurrent.futures import ThreadPoolExecutor
import hashlib
import json
import os
import re
from pathlib import Path
import shlex
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[1]
# name: card, movement interval, direction, shared route before the comparison
SCENES = {
    "field": ("disc1-start-maxed.mcr", 1700, 1900, "DOWN", []),
    "infirmary": ("disc1-start-maxed.mcr", 2400, 2600, "RIGHT", [(1700, 1900, "DOWN")]),
    "garden-hall": ("disc1-start-maxed.mcr", 3000, 3200, "DOWN",
                    [(1700, 1900, "DOWN"), (2400, 2600, "RIGHT")]),
    "world": ("disc1-early.mcr", 1700, 1900, "RIGHT", []),
}
DIRECTIONS = {"UP": (0x10, 1, -32768), "RIGHT": (0x20, 0, 32767),
              "DOWN": (0x40, 1, 32767), "LEFT": (0x80, 0, -32768)}


def write_sequence(path, first, last, direction, route, mode):
    neutral = (0, 0, 0, 0, 0)
    events = {1: neutral}
    for i, start in enumerate((300, 600, 900, 1140, 1300, 1460)):
        events[start] = (0x4000, 0, 0, 0, 0)
        events[start + (2 if i < 3 else 8)] = neutral
    for start, end, button in route:
        events[start] = (DIRECTIONS[button][0], 0, 0, 0, 0)
        events[end] = neutral
    bit, axis, value = DIRECTIONS[direction]
    state = list(neutral)
    if mode == "digital":
        state[0] = bit
    elif mode == "axis":
        state[axis + 1] = value
    events[first], events[last] = state, neutral
    path.write_text("".join(f"{frame} {buttons:04x} {' '.join(map(str, axes))}\n"
                            for frame, (buttons, *axes) in sorted(events.items())))


def field_positions(text, first, last):
    pattern = r"PAD_FIELD vblank=(\d+) map=([a-z0-9_]+) x=(-?\d+) y=(-?\d+) z=(-?\d+) facing=(\d+)"
    return {int(frame): (name, int(x), int(y), int(z))
            for frame, name, x, y, z, _ in re.findall(pattern, text) if first <= int(frame) <= last}


def walking_changed(idle, moved):
    # Facing/animation and different dispatcher PCs do not prove translation.
    # Field assets may be unavailable for some samples during a transition.
    common = idle.keys() & moved.keys()
    return any(moved[f] != idle[f] for f in common)


def protocol_matches(polls, expected, first, last):
    # Guest transactions need not occur exactly once per VBlank.
    return len(polls) >= (last - first) // 2 and all(rx == expected for _, rx in polls)



def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


