"""A one-line progress bar, drawn only on a terminal."""
import sys
import time

from .util import human


class Progress(object):
    def __init__(self, label, stream=None, enabled=None, width=24):
        self.label = label
        self.stream = stream or sys.stderr
        if enabled is None:
            enabled = hasattr(self.stream, "isatty") and self.stream.isatty()
        self.enabled = enabled
        self.width = width
        self.start = time.monotonic()
        self.last = 0.0
        self.drawn = False

    def __call__(self, done, total):
        if not self.enabled:
            return
        now = time.monotonic()
        if done < total and now - self.last < 0.1:
            return
        self.last = now
        frac = (float(done) / total) if total else 1.0
        fill = int(round(frac * self.width))
        speed = done / max(now - self.start, 0.001)
        line = "\r{:<28.28} [{}{}] {:3d}%  {}/s ".format(
            self.label, "#" * fill, "." * (self.width - fill), int(frac * 100), human(speed))
        self.stream.write(line)
        self.stream.flush()
        self.drawn = True

    def done(self):
        if self.drawn:
            self.stream.write("\r" + " " * 79 + "\r")
            self.stream.flush()
            self.drawn = False
