"""Small helpers shared by the commands."""
import posixpath
import re

from .errors import RpcError

_PLAIN = re.compile(r"^[A-Za-z0-9_./@%+=:,-]+$")


def quote(arg):
    """Quote one argument for the device's shell.

    The shell honours '...' and "..." (a quote opens only where a word begins)
    and has no escape character, so an argument holding both kinds of quote
    cannot be written at all and is refused here rather than mangled."""
    if arg and _PLAIN.match(arg):
        return arg
    if '"' not in arg:
        return '"{}"'.format(arg)
    if "'" not in arg:
        return "'{}'".format(arg)
    raise RpcError("{!r} holds both kinds of quote, which the device's shell cannot be given"
                   .format(arg))


def rjoin(*parts):
    """Join device paths."""
    return posixpath.join(*parts)


def rnorm(path, cwd="/"):
    """An absolute, normalised device path."""
    if not path:
        path = "."
    if not path.startswith("/"):
        path = posixpath.join(cwd or "/", path)
    path = posixpath.normpath(path)
    if path.startswith("//"):
        path = "/" + path.lstrip("/")
    return path


def human(n):
    n = float(n)
    for unit in ("B", "KB", "MB"):
        if n < 1024 or unit == "MB":
            return ("{:.0f} {}" if unit == "B" else "{:.1f} {}").format(n, unit)
        n /= 1024.0
    return "{:.1f} MB".format(n)


def rate(nbytes, seconds):
    return "{}/s".format(human(nbytes / max(seconds, 0.001)))
