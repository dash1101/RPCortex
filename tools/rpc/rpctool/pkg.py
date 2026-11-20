"""Installing a package file that is on the computer."""
import re

from .errors import RpcError
from .util import quote


def tmp_name(name):
    base = re.sub(r"\.app$", "", name, flags=re.I)
    base = re.sub(r"[^A-Za-z0-9_.-]", "_", base) or "package"
    return "/tmp/{}.app".format(base[:40])


def install_bytes(dev, data, name, progress=None, timeout=None):
    """Copy a package to /tmp on the device, `pkg install` it, and remove the
    copy again whatever happened. Returns the install's ExecResult."""
    if not data:
        raise RpcError("{} is empty".format(name))
    tmp = tmp_name(name)
    dev.put(data, tmp, progress)
    try:
        # With sudo where the firmware has it: installing changes the system.
        return dev.exec(dev.sudo_line("pkg install " + quote(tmp)), timeout)
    finally:
        try:
            dev.exec("rm " + quote(tmp))
        except (RpcError, OSError):
            pass
