"""rpctool: drive an RPCortex v2 device from a computer, over its serial console.

    from rpctool import Device
    with Device.open() as dev:
        print(dev.exec("ls /home").text)

The protocol is LINK.md at the root of the RPCortex v2 repository.
"""
__version__ = "1.0.0"

from .errors import (DeviceGone, DeviceTimeout, DigestMismatch, LinkError,  # noqa: F401
                     LoginFailed, LoginRequired, NotSupported, ProtocolError, RpcError)
from .link import Entry, ExecResult, LinkClient, Timing  # noqa: F401
from .device import Device  # noqa: F401
from .transport import Transport, choose_port, list_candidates, open_serial  # noqa: F401
