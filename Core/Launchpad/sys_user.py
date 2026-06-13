# Desc: User management shell commands - RPCortex Pulsar OS
# File: /Core/Launchpad/sys_user.py
# Last Updated: 6/12/2026
# Lang: MicroPython, English
# Version: v0.9.1

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
    """Modify a user account (passwords, name, nopass, admin) in one command.

    Usage:
      usermod <user> passwd            change the password
      usermod <user> rename <newname>  rename the account
      usermod <user> admin  on|off     grant / revoke admin rights
      usermod <user> nopass on|off     enable / disable no-password login
    """
    toks = (args or '').split()
    if len(toks) < 2:
        warn("Usage: usermod <user> passwd | rename <name> | admin on|off | nopass on|off")
        return
    from usrmgmt import (decode as _decode, set_admin, set_nopass, set_password,
                         rename_user, change_password, require_admin)
    user   = toks[0]
    sub    = toks[1].lower()
    active = regedit.read('Settings.Active_User')

    if not _decode(user, silent=True):
        error("User '{}' not found.".format(user))
        return

    # --- change password ---
    if sub in ('passwd', 'password', 'pw'):
        if user == active:
            change_password(user)               # self-service: verifies current pw
        else:
            if not require_admin("change another user's password"):
                return
            new_pw = masked_inpt("New password for '{}'".format(user))
            if not new_pw.strip():
                warn("Password cannot be blank.")
                return
            if new_pw != masked_inpt("Confirm new password"):
                error("Passwords do not match.")
                return
            set_password(user, new_pw)
        return

    # --- rename ---
    if sub in ('rename', 'name'):
        if len(toks) < 3:
            warn("Usage: usermod <user> rename <newname>")
            return
        if user == active:
            error("Cannot rename the active user. Log in as another admin first.")
            return
        if not require_admin("rename a user account"):
            return
        rename_user(user, toks[2])
        return

    # --- admin on/off ---
    if sub == 'admin':
        if len(toks) < 3 or toks[2].lower() not in ('on', 'off'):
            warn("Usage: usermod <user> admin on|off")
            return
        if not require_admin("change admin rights"):
            return
        set_admin(user, toks[2].lower() == 'on')
        return

    # --- nopass on/off ---
    if sub == 'nopass':
        if len(toks) < 3 or toks[2].lower() not in ('on', 'off'):
            warn("Usage: usermod <user> nopass on|off")
            return
        if not require_admin("change no-password login"):
            return
        if toks[2].lower() == 'on':
            set_nopass(user, True)
        else:
            new_pw = masked_inpt("Set a password for '{}'".format(user))
            if not new_pw.strip():
                warn("A password is required to disable nopass.")
                return
            if new_pw != masked_inpt("Confirm password"):
                error("Passwords do not match.")
                return
            set_nopass(user, False, new_password=new_pw)
        return

    warn("Unknown action '{}'. Use: passwd | rename | admin | nopass".format(sub))


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


def logout(args=None):
    state = globals().get('_shell_state')
    if state:
        state['running'] = False


def exit(args=None):
    logout(args)
