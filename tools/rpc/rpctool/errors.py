"""The errors rpctool raises. Every one is an RpcError, so a script can catch
that one name and print the message: each is written to be shown to a person."""


class RpcError(Exception):
    """Something went wrong talking to the device."""


class DeviceGone(RpcError):
    """The port went away: the board rebooted, was unplugged, or reset."""


class DeviceTimeout(RpcError):
    """The device stopped answering within the time allowed."""


class LoginRequired(RpcError):
    """The device is at a login prompt and no account was given."""


class LoginFailed(RpcError):
    """The device refused the account or the password."""


class NotSupported(RpcError):
    """The firmware on the device cannot do this (it predates link mode)."""


class ProtocolError(RpcError):
    """The device answered something the protocol does not allow."""


class LinkError(RpcError):
    """The device answered a request with ERR <code> <message>."""

    def __init__(self, code, message):
        self.code = code
        self.message = message
        RpcError.__init__(self, "{} ({})".format(message or code, code))


class DigestMismatch(RpcError):
    """What arrived is not what was sent."""
