# Desc: User management shell commands - RPCortex Nebula OS
# File: /Core/Launchpad/sys_user.py
# Last Updated: 4/1/2026
# Lang: MicroPython, English
# Version: v0.8.1-beta4

import sys
if '/Core' not in sys.path:
    sys.path.append('/Core')

import regedit
from RPCortex import warn, error, info, ok, multi, inpt


def whoami(args=None):
    user = regedit.read('Settings.Active_User')
    multi(user if user else "(unknown)")


def mkacct(args=None):
    from usrmgmt import add_user
    info("Create a new user account.")
    username = inpt("Username").strip()
    if not username:
        warn("Username cannot be blank.")
        return
    password = inpt("Password")
    if not password.strip():
        warn("Password cannot be blank.")
        return
    confirm = inpt("Confirm password")
    if password != confirm:
        error("Passwords do not match.")
        return
    if add_user(username, password):
        ok("Account '{}' created successfully.".format(username))
    else:
        error("Failed to create account '{}'.".format(username))


def rmuser(args):
    if not args:
        warn("Usage: rmuser <username>")
        return
    from usrmgmt import rmuser as _rm, decode as _decode, is_nopass as _is_nopass
    target = args.strip()
    active = regedit.read('Settings.Active_User')
    if target == active:
        error("Cannot remove the currently active user.")
        return
    if not _decode(target, silent=True):
        error("User '{}' not found.".format(target))
        return
    # Non-root must verify the target account's password
    if active != 'root':
        if not _is_nopass(target):
            pw = inpt("Enter password for '{}' to confirm".format(target))
            if not _decode(target, pw, silent=True):
                error("Incorrect password. Cannot remove user.")
                return
    confirm = inpt("Remove user '{}' ? (yes/no)".format(target))
    if confirm.strip().lower() == 'yes':
        _rm(target)
    else:
        info("Cancelled.")


def chpswd(args):
    if not args:
        warn("Usage: chpswd <username>")
        return
    from usrmgmt import change_password
    change_password(args.strip())


def logout(args=None):
    state = globals().get('_shell_state')
    if state:
        state['running'] = False


def exit(args=None):
    logout(args)
