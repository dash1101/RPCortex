# Desc: User management shell commands - RPCortex Pulsar OS
# File: /Core/Launchpad/sys_user.py
# Last Updated: 6/9/2026
# Lang: MicroPython, English
# Version: v0.8.2

import sys
if '/Core' not in sys.path:
    sys.path.append('/Core')

import regedit
from RPCortex import warn, error, info, ok, multi, inpt, masked_inpt


def whoami(args=None):
    user = regedit.read('Settings.Active_User')
    multi(user if user else "(unknown)")


_PROTECTED = ('root', 'guest')   # can never be removed


def users(args=None):
    """List all user accounts on the device."""
    from usrmgmt import list_users
    active = regedit.read('Settings.Active_User')
    accounts = list_users()
    if not accounts:
        warn("No user accounts found.")
        return
    info("User accounts ({}):".format(len(accounts)))
    for name, nopass, home, admin in accounts:
        tags = []
        if name == active:
            tags.append('active')
        if admin:
            tags.append('admin')
        if nopass:
            tags.append('nopass')
        tag_str = '  [{}]'.format(', '.join(tags)) if tags else ''
        multi("  {:<16} {}{}".format(name, home or '-', tag_str))


def mkacct(args=None):
    """Create a user.  Usage: mkacct [username] [--nopass] [--admin]"""
    from usrmgmt import add_user, require_admin
    toks   = (args or '').split()
    nopass = '--nopass' in toks
    admin  = '--admin' in toks
    names  = [t for t in toks if not t.startswith('--')]

    info("Create a new user account.")
    username = names[0] if names else inpt("Username").strip()
    if not username:
        warn("Username cannot be blank.")
        return

    # Only admins may grant admin rights to a new account.
    if admin and not require_admin("create an admin account"):
        return

    if nopass:
        password = ''           # NOPASS account — no password prompt
    else:
        password = masked_inpt("Password")
        if not password.strip():
            warn("Password cannot be blank (use --nopass for a no-password account).")
            return
        confirm = masked_inpt("Confirm password")
        if password != confirm:
            error("Passwords do not match.")
            return

    if add_user(username, password, nopass=nopass, admin=admin):
        ok("Account '{}' created{}{}.".format(
            username, ' (admin)' if admin else '', ' (nopass)' if nopass else ''))
    else:
        error("Failed to create account '{}'.".format(username))


def usermod(args):
    """Grant/revoke admin.  Usage: usermod <user> admin on|off"""
    toks = (args or '').split()
    if len(toks) < 3 or toks[1].lower() != 'admin' or toks[2].lower() not in ('on', 'off'):
        warn("Usage: usermod <user> admin on|off")
        return
    from usrmgmt import set_admin, require_admin, decode as _decode
    user = toks[0]
    if not _decode(user, silent=True):
        error("User '{}' not found.".format(user))
        return
    if not require_admin("change admin rights"):
        return
    set_admin(user, toks[2].lower() == 'on')


def rmuser(args):
    if not args:
        warn("Usage: rmuser <username>")
        return
    from usrmgmt import rmuser as _rm, decode as _decode, is_nopass as _is_nopass
    target = args.strip()
    active = regedit.read('Settings.Active_User')
    if target in _PROTECTED:
        error("'{}' is a protected account and cannot be removed.".format(target))
        return
    if target == active:
        error("Cannot remove the currently active user.")
        return
    if not _decode(target, silent=True):
        error("User '{}' not found.".format(target))
        return
    # Non-root must verify the target account's password
    if active != 'root':
        if not _is_nopass(target):
            pw = masked_inpt("Enter password for '{}' to confirm".format(target))
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
