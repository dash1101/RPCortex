# Desc: User management — accounts, passwords, authentication for RPCortex - Pulsar OS
# File: /Core/usrmgmt.py
# Last Updated: 6/9/2026
# Lang: MicroPython, English
# Version: v0.8.2
# Author: dash1101
#
# Password storage format (user.cfg):
#   'username', 'salt$sha256(salt+password)', '/path/'
#
# Special stored hash values:
#   'NOPASS'  — account accepts any password including blank (used by guest)
#
# Legacy format (no '$' in hash field) is detected and still accepted so that
# accounts created before v0.8.2 continue to work.  Passwords are upgraded to
# the salted format the next time change_password() is called.

import hashlib
import os

_CFG = "/Pulsar/Registry/user.cfg"

# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------

def _gen_salt():
    """Generate a 16-char hex salt using the hardware RNG."""
    raw = os.urandom(8)
    return ''.join('{:02x}'.format(b) for b in raw)

def _sha256_hex(data):
    h = hashlib.sha256()
    h.update(data if isinstance(data, bytes) else data.encode())
    d = h.digest()
    return ''.join('{:02x}'.format(b) for b in d)

def hash_password(password, salt=None):
    """Return 'salt$hash' for storage.  Generates salt if not provided."""
    if salt is None:
        salt = _gen_salt()
    digest = _sha256_hex(salt + password)
    return salt + '$' + digest

def verify_password(password, stored):
    """
    Verify a plaintext password against a stored hash.
    Handles:
      - 'NOPASS'       : always returns True (guest-style accounts)
      - 'salt$hash'    : salted SHA256 (current format)
      - bare sha256    : legacy unsalted format
    """
    if stored == 'NOPASS':
        return True
    if '$' in stored:
        salt, _ = stored.split('$', 1)
        return hash_password(password, salt) == stored
    else:
        # Legacy: unsalted SHA256 — accept but do not upgrade here
        return _sha256_hex(password) == stored

def _backup(path):
    bak = path + ".bak"
    try:
        with open(path, 'r') as f:
            content = f.read()
        with open(bak, 'w') as f:
            f.write(content)
    except OSError:
        try:
            with open(bak, 'w') as f:
                f.write("")
        except OSError:
            pass
    return bak

def _restore(path, bak):
    try:
        with open(bak, 'r') as f:
            content = f.read()
        with open(path, 'w') as f:
            f.write(content)
        os.remove(bak)
    except OSError:
        pass

def _ensure_user_dir(username):
    """Create /Users/<username>/ if it doesn't exist."""
    try:
        os.stat('/Users')
    except OSError:
        try:
            os.mkdir('/Users')
        except OSError:
            pass
    try:
        os.mkdir('/Users/{}'.format(username))
    except OSError:
        pass  # already exists — not an error

# ---------------------------------------------------------------------------
# Output helpers (lazy import to avoid circular dependency at module load)
# ---------------------------------------------------------------------------

def _ok(msg):
    try:
        from RPCortex import ok
        ok(msg)
    except Exception:
        print("[OK] " + msg)

def _warn(msg):
    try:
        from RPCortex import warn
        warn(msg)
    except Exception:
        print("[WARN] " + msg)

def _error(msg):
    try:
        from RPCortex import error
        error(msg)
    except Exception:
        print("[ERR] " + msg)

def _inpt(prompt):
    try:
        from RPCortex import inpt
        return inpt(prompt)
    except Exception:
        return input(prompt + ": ")

def _minpt(prompt):
    """Masked input for password prompts; falls back to plain input."""
    try:
        from RPCortex import masked_inpt
        return masked_inpt(prompt)
    except Exception:
        return input(prompt + ": ")

# ---------------------------------------------------------------------------
# Public API
# ---------------------------------------------------------------------------

def add_user(username, password, nopass=False, admin=False):
    """
    Add a new user account.
    - nopass=True  creates a NOPASS account (accepts any password / blank)
    - admin=True   grants administrator rights (root is always an admin)
    - Creates /Users/<username>/ home directory automatically.
    Returns True on success, False if already exists or error.

    user.cfg line: 'name', 'salt$hash', '/home/', 'admin'|'user'
    (the 4th field is optional for backward compatibility — a line without it
    is treated as admin only when the username is 'root'.)
    """
    bak = _backup(_CFG)
    try:
        # Read existing
        try:
            with open(_CFG, 'r') as f:
                lines = f.readlines()
        except OSError:
            lines = []

        # Duplicate check
        for line in lines:
            parts = line.strip().split(', ')
            if parts and parts[0] == "'{}'".format(username):
                _warn("User '{}' already exists.".format(username))
                return False

        user_path = '/Users/{}/'.format(username)
        hashed    = 'NOPASS' if nopass else hash_password(password)
        role      = 'admin' if (admin or username == 'root') else 'user'

        with open(_CFG, 'a') as f:
            f.write("'{}', '{}', '{}', '{}'\n".format(username, hashed, user_path, role))

        _ensure_user_dir(username)
        _ok("Account '{}' created.".format(username))
        return True

    except Exception as e:
        _error("Failed to create account: {}".format(e))
        _restore(_CFG, bak)
        return False
    finally:
        try:
            os.remove(bak)
        except OSError:
            pass


def is_nopass(username):
    """Return True if the account has NOPASS marker (i.e. no password required)."""
    try:
        with open(_CFG, 'r') as f:
            for line in f:
                parts = line.strip().split(', ')
                if len(parts) >= 2 and parts[0] == "'{}'".format(username):
                    return parts[1][1:-1] == 'NOPASS'
    except OSError:
        pass
    return False


def is_admin(username):
    """Return True if the account is an administrator (root always is)."""
    if username == 'root':
        return True
    try:
        with open(_CFG, 'r') as f:
            for line in f:
                parts = line.strip().split(', ')
                if len(parts) >= 4 and parts[0] == "'{}'".format(username):
                    return parts[3][1:-1] == 'admin'
    except OSError:
        pass
    return False


def set_admin(username, admin):
    """Grant (admin=True) or revoke (False) admin rights for a user."""
    if username == 'root' and not admin:
        _warn("root is always an administrator.")
        return False
    bak = _backup(_CFG)
    try:
        with open(_CFG, 'r') as f:
            lines = f.readlines()
        role = 'admin' if admin else 'user'
        found = False
        out = []
        for line in lines:
            parts = line.strip().split(', ')
            if parts and parts[0] == "'{}'".format(username):
                found = True
                name = parts[0]
                h    = parts[1] if len(parts) >= 2 else "''"
                home = parts[2] if len(parts) >= 3 else "'/Users/{}/'".format(username)
                out.append("{}, {}, {}, '{}'\n".format(name, h, home, role))
            else:
                out.append(line if line.endswith('\n') else line + '\n')
        if not found:
            _error("User '{}' not found.".format(username))
            return False
        with open(_CFG, 'w') as f:
            f.write(''.join(out))
        _ok("'{}' is now {}.".format(username, 'an administrator' if admin else 'a standard user'))
        return True
    except Exception as e:
        _error("Could not update role: {}".format(e))
        _restore(_CFG, bak)
        return False
    finally:
        try:
            os.remove(bak)
        except OSError:
            pass


def require_admin(reason=''):
    """Gate a privileged action: the active user must be an admin AND prove it.

    - non-admin            -> denied.
    - admin with a password -> must re-enter it.
    - admin with NOPASS     -> a yes/no confirmation (no password to check).
    Returns True if authorized.
    """
    import regedit
    active = regedit.read('Settings.Active_User') or ''
    if not is_admin(active):
        _error("This action needs administrator rights{}.".format(
            (' (' + reason + ')') if reason else ''))
        _warn("Log in as an admin account (e.g. root).")
        return False
    if is_nopass(active):
        ans = _inpt("Admin '{}' has no password. Type YES to confirm".format(active))
        return ans.strip() == 'YES'
    pw = _minpt("Confirm admin password for '{}'".format(active))
    if decode(active, pw, silent=True):
        return True
    _error("Incorrect password — action cancelled.")
    return False


def list_users():
    """Return a list of (username, nopass, home, admin) tuples from user.cfg."""
    out = []
    try:
        with open(_CFG, 'r') as f:
            for line in f:
                parts = line.strip().split(', ')
                if len(parts) >= 2 and parts[0]:
                    name = parts[0][1:-1]   # strip surrounding quotes
                    h    = parts[1][1:-1]
                    home = parts[2][1:-1] if len(parts) >= 3 else ''
                    admin = (parts[3][1:-1] == 'admin') if len(parts) >= 4 else (name == 'root')
                    if name:
                        out.append((name, h == 'NOPASS', home, admin))
    except OSError:
        pass
    return out


def change_password(username):
    """Interactive password change for an existing user."""
    bak = _backup(_CFG)
    try:
        with open(_CFG, 'r') as f:
            lines = f.readlines()

        new_lines = []
        found = False
        for line in lines:
            parts = line.strip().split(', ')
            if parts and parts[0] == "'{}'".format(username):
                found = True
                stored_hash = parts[1][1:-1]   # strip surrounding quotes
                if stored_hash != 'NOPASS':
                    old_pw = _minpt("Current password")
                    if not verify_password(old_pw, stored_hash):
                        _error("Incorrect current password.")
                        return
                new_pw = _minpt("New password")
                if not new_pw.strip():
                    _warn("Password cannot be blank.")
                    return
                confirm = _minpt("Confirm new password")
                if new_pw != confirm:
                    _error("Passwords do not match.")
                    return
                user_path = parts[2][1:-1]
                new_lines.append("'{}', '{}', '{}'\n".format(
                    username, hash_password(new_pw), user_path))
            else:
                new_lines.append(line)

        if not found:
            _error("User '{}' not found.".format(username))
            return

        with open(_CFG, 'w') as f:
            f.write(''.join(new_lines))
        _ok("Password for '{}' updated.".format(username))

    except Exception as e:
        _error("Password change failed: {}".format(e))
        _restore(_CFG, bak)
    finally:
        try:
            os.remove(bak)
        except OSError:
            pass


def rmuser(username):
    """Remove a user account by name."""
    bak = _backup(_CFG)
    try:
        with open(_CFG, 'r') as f:
            lines = f.readlines()

        new_lines = [l for l in lines
                     if not (l.strip().split(', ') and
                             l.strip().split(', ')[0] == "'{}'".format(username))]

        if len(new_lines) == len(lines):
            _error("User '{}' not found.".format(username))
            return

        with open(_CFG, 'w') as f:
            f.write(''.join(new_lines))
        _ok("User '{}' removed.".format(username))

    except Exception as e:
        _error("Failed to remove user: {}".format(e))
        _restore(_CFG, bak)
    finally:
        try:
            os.remove(bak)
        except OSError:
            pass


def decode(username, password=None, silent=False):
    """
    Verify a user exists (and optionally their password).
    Returns True/False.  Silent suppresses all output.
    Handles NOPASS accounts, salted SHA256, and legacy bare-SHA256.
    """
    try:
        with open(_CFG, 'r') as f:
            lines = f.readlines()

        for line in lines:
            parts = line.strip().split(', ')
            if not parts or parts[0] != "'{}'".format(username):
                continue

            if password is not None:
                stored = parts[1][1:-1]   # strip quotes
                if verify_password(password, stored):
                    if not silent:
                        _ok("Login verified: {}".format(username))
                    return True
                else:
                    if not silent:
                        _error("Incorrect password.")
                    return False
            else:
                if not silent:
                    _ok("User '{}' exists.".format(username))
                return True

        if not silent:
            _warn("User '{}' not found.".format(username))
        return False

    except OSError:
        if not silent:
            _error("Cannot read user configuration file.")
        return False


def login_seq():
    """
    Standalone login loop — fallback used when shell state is unavailable.
    The main boot path uses initialization.login_seq() instead.
    """
    import sys
    if '/Core' not in sys.path:
        sys.path.append('/Core')
    from launchpad import launchpad_init

    while True:
        username = _inpt("Username").strip()
        if not username:
            _warn("Username cannot be blank.")
            continue

        if not decode(username, silent=True):
            _warn("User '{}' not found.".format(username))
            continue

        # NOPASS account — no password prompt
        if is_nopass(username):
            _ok("Welcome, {}!".format(username))
            launchpad_init(username, '')
            return

        password = _inpt("Password").strip()
        if not password:
            _warn("Password cannot be blank.")
            continue

        if decode(username, password, silent=True):
            launchpad_init(username, password)
            return
        else:
            _error("Incorrect password.")
