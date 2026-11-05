#!/usr/bin/env python3
"""Procedural mathematical video sources for the Autoclip mixer."""

import time

import cv2
import numpy as np


class SquircleSource:
    """Vectorized CPU adaptation of XorDev's lightweight Squircle 2."""

    NAME = 'SQUIRCLE'
    LOOP_SECONDS = float(8.0 * np.pi)

    def __init__(self, out_w=720, out_h=576, render_w=240, render_h=192):
        self.out_w = out_w
        self.out_h = out_h
        self.render_w = render_w
        self.render_h = render_h
        self.started = time.monotonic()
        self.last_render = -1.0
        self.frame = np.zeros((out_h, out_w, 3), dtype=np.uint8)
        self.small = np.zeros((render_h, render_w, 3), dtype=np.uint8)

        x = np.arange(render_w, dtype=np.float32) + 0.5
        y = np.arange(render_h, dtype=np.float32) + 0.5
        xx, yy = np.meshgrid(x, y)
        self.px = (xx * 2.0 - render_w) / render_h
        self.py = (yy * 2.0 - render_h) / render_h

    def render(self):
        now = time.monotonic()
        # A source may be requested by more than one mixer role in one frame.
        if now - self.last_render < 1.0 / 60.0:
            return self.frame
        self.last_render = now
        t = np.float32((now - self.started) % self.LOOP_SECONDS)

        angle = t / 4.0 - np.sin(t) / 4.0
        c0 = np.cos(angle)
        # GLSL's vec * mat2 uses the two matrix columns for output x/y.
        cx = self.px * c0 + self.py * np.cos(angle + 33.0)
        cy = self.px * np.cos(angle + 11.0) + self.py * c0

        # sqrt(length(c*c)) is the fourth root of cx^4 + cy^4.
        squircle_distance = np.sqrt(np.hypot(cx * cx, cy * cy))
        wave = np.cos(squircle_distance / 0.1 - t)
        denominator = np.maximum(wave / 0.2, -wave)
        denominator = np.maximum(denominator, 1.0e-6)

        radius = np.hypot(cx, cy)
        phase = 3.0 * radius + 2.0 * cy + t
        for channel, offset in enumerate((6.0, 1.0, 2.0)):
            color_wave = np.sin(phase + offset)
            color = np.tanh((0.6 + 0.5 * color_wave) / denominator)
            self.small[:, :, channel] = (
                np.clip(color, 0.0, 1.0) * 255.0).astype(np.uint8)
        cv2.resize(self.small, (self.out_w, self.out_h), dst=self.frame,
                   interpolation=cv2.INTER_LINEAR)
        return self.frame
