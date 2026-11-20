"""Every `rpc` command, run through the real argument parser against the mock."""
import hashlib
import json
import os
import shutil
import sys
import tempfile
import time
import unittest
import zipfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import support  # noqa: E402
from support import mock, run_cli  # noqa: E402
from mockdev import MockDevice, make_app  # noqa: E402
from rpctool import flash, transport  # noqa: E402

SNOW = "naïve ☃"


class CliCase(unittest.TestCase):
    mock_args = {}

    def setUp(self):
        self.dev = MockDevice(**self.mock_args).start()
        self.addCleanup(self.dev.shutdown)
        self.tmp = tempfile.mkdtemp(prefix="rpctest-")
        self.addCleanup(shutil.rmtree, self.tmp, True)
        self.cwd = os.getcwd()
        os.chdir(self.tmp)
        self.addCleanup(os.chdir, self.cwd)

    # The account's password, for the sudo that installing and writing /etc
    # needs on current firmware. TestSudo takes it away to see what happens.
    env = {"RPC_PASSWORD": "rootpw"}

    def rpc(self, *argv):
        rc, out, err = run_cli(self.dev, argv, env=self.env)
        time.sleep(0.05)
        self.assertFalse(self.dev.in_link, "rpc {} left the device in link mode".format(argv))
        return rc, out, err

    def local(self, name, data):
        p = os.path.join(self.tmp, name)
        d = os.path.dirname(p)
        if not os.path.isdir(d):
            os.makedirs(d)
        with open(p, "wb") as f:
            f.write(data)
        return p


class TestBasics(CliCase):
    def test_info(self):
        rc, out, err = self.rpc("info")
        text = out.decode()
        self.assertEqual(rc, 0, err)
        for want in ("link (protocol 2)", "pico2_w", "v2.01-beta", "memory", "280 KB free",
                     "largest block 180 KB", "storage"):
            self.assertIn(want, text)

    def test_exec_mirrors_status(self):
        rc, out, _ = self.rpc("exec", "echo hello")
        self.assertEqual((rc, out), (0, b"hello\n"))
        rc, out, _ = self.rpc("exec", "fail")
        self.assertEqual(rc, 1)
        self.assertIn(b"[!] it failed", out)
        rc, out, _ = self.rpc("exec", "echo", "two", "words")
        self.assertEqual(out, b"two words\n")

    def test_exec_timeout(self):
        rc, out, err = self.rpc("exec", "--timeout", "0.3", "sleep 20")
        self.assertEqual(rc, 124)
        self.assertIn("timed out", err)

    def test_exec_truncated_is_said(self):
        rc, out, err = self.rpc("exec", "big 40000")
        self.assertIn("cut off", err)

    def test_ls_stat(self):
        rc, out, _ = self.rpc("ls", "/home/root")
        self.assertEqual(rc, 0)
        self.assertIn(b"notes.txt", out)
        rc, out, _ = self.rpc("ls")
        self.assertIn(b"home/", out)
        rc, out, _ = self.rpc("stat", "/home/root/notes.txt")
        self.assertEqual((rc, out), (0, b"/home/root/notes.txt  file  22 bytes\n"))
        rc, out, err = self.rpc("stat", "/nope")
        self.assertEqual(rc, 1)
        self.assertIn("no such", err)
        rc, out, err = self.rpc("ls", "/nope")
        self.assertEqual(rc, 1)
        self.assertIn("[nofile]", err)

    def test_log_ps(self):
        rc, out, _ = self.rpc("log")
        self.assertIn(b"boot ok", out)
        rc, out, _ = self.rpc("ps")
        self.assertIn(b"shell", out)

    def test_rm_mkdir_mv_cat(self):
        self.assertEqual(self.rpc("mkdir", "-p", "/home/root/" + SNOW + "/deep")[0], 0)
        self.assertTrue(self.dev.fs.isdir("/home/root/" + SNOW + "/deep"))
        rc, _, err = self.rpc("mkdir", "/home/root/" + SNOW)
        self.assertEqual(rc, 1)
        self.assertEqual(self.rpc("mv", "/home/root/notes.txt", "/home/root/" + SNOW + "/n.txt")[0], 0)
        rc, out, _ = self.rpc("cat", "/home/root/" + SNOW + "/n.txt")
        self.assertEqual(out, b"hello from the device\n")
        rc, _, err = self.rpc("rm", "/home/root/" + SNOW)
        self.assertEqual(rc, 1)
        self.assertEqual(self.rpc("rm", "-r", "/home/root/" + SNOW)[0], 0)
        self.assertFalse(self.dev.fs.exists("/home/root/" + SNOW))
        rc, _, err = self.rpc("rm", "/home/root/*")
        self.assertEqual(rc, 1)
        self.assertIn("wildcard", err)
        self.assertIn("/home/root/secret.txt", self.dev.fs.files)

    def test_wget(self):
        rc, out, _ = self.rpc("wget", "http://example.invalid/a.txt")
        self.assertEqual(rc, 1)
        self.assertIn(b"No network", out)
        self.dev.online = True
        self.dev.web["http://example.invalid/a.txt"] = b"from the web"
        rc, out, _ = self.rpc("wget", "http://example.invalid/a.txt", "/tmp/a b.txt")
        self.assertEqual(rc, 0)
        self.assertEqual(self.dev.fs.files["/tmp/a b.txt"], b"from the web")

    def test_watch(self):
        rc, out, _ = self.rpc("watch", "--every", "0.05", "--count", "2", "echo tick")
        self.assertEqual(rc, 0)
        self.assertEqual(out.count(b"tick\n"), 2)

    def test_reboot(self):
        gen = self.dev.gen
        rc, out, _ = run_cli(self.dev, ["reboot"])
        self.assertEqual(rc, 0)
        self.assertIn(b"Rebooting", out)
        self.assertEqual(self.dev.gen, gen + 1)

    def test_no_device(self):
        rc, out, err = run_cli(None, ["info"])
        self.assertEqual(rc, 1)
        self.assertIn("No RPCortex device found", err)

    def test_usage(self):
        with self.assertRaises(SystemExit) as cm:
            run_cli(self.dev, ["put"])
        self.assertEqual(cm.exception.code, 2)


class TestPutGet(CliCase):
    def test_put_get_file(self):
        data = os.urandom(3000)
        src = self.local("blob.bin", data)
        rc, _, err = self.rpc("put", src)
        self.assertEqual(rc, 0, err)
        self.assertEqual(self.dev.fs.files["/blob.bin"], data)
        self.assertIn("sha256 verified", err)
        rc, _, _ = self.rpc("put", src, "/home/root")
        self.assertEqual(self.dev.fs.files["/home/root/blob.bin"], data)
        rc, _, _ = self.rpc("put", src, "/tmp/" + SNOW + ".bin")
        self.assertEqual(self.dev.fs.files["/tmp/" + SNOW + ".bin"], data)
        rc, _, err = self.rpc("get", "/tmp/" + SNOW + ".bin", "back.bin")
        self.assertEqual(rc, 0, err)
        with open("back.bin", "rb") as f:
            self.assertEqual(f.read(), data)
        rc, _, _ = self.rpc("get", "/home/root/notes.txt")
        with open("notes.txt", "rb") as f:
            self.assertEqual(f.read(), b"hello from the device\n")
        os.mkdir("into")
        self.rpc("get", "/home/root/notes.txt", "into")
        self.assertTrue(os.path.isfile(os.path.join("into", "notes.txt")))

    def test_empty_file(self):
        src = self.local("empty", b"")
        self.assertEqual(self.rpc("put", src, "/tmp/empty")[0], 0)
        self.assertEqual(self.dev.fs.files["/tmp/empty"], b"")

    def test_trees(self):
        self.local("tree/a.txt", b"a")
        self.local("tree/sub dir/b.txt", b"bb")
        self.local("tree/sub dir/" + SNOW + "/c.txt", b"ccc")
        os.makedirs(os.path.join(self.tmp, "tree", "empty"))
        rc, _, err = self.rpc("put", "tree", "/home/root/t")
        self.assertEqual(rc, 1)
        self.assertIn("-r", err)
        rc, _, err = self.rpc("put", "-r", "tree", "/home/root/t")
        self.assertEqual(rc, 0, err)
        fs = self.dev.fs
        self.assertEqual(fs.files["/home/root/t/sub dir/" + SNOW + "/c.txt"], b"ccc")
        self.assertTrue(fs.isdir("/home/root/t/empty"))
        rc, _, err = self.rpc("get", "-r", "/home/root/t", "pulled")
        self.assertEqual(rc, 0, err)
        with open(os.path.join("pulled", "sub dir", SNOW, "c.txt"), "rb") as f:
            self.assertEqual(f.read(), b"ccc")
        self.assertTrue(os.path.isdir(os.path.join("pulled", "empty")))
        rc, _, err = self.rpc("get", "/home/root/t")
        self.assertEqual(rc, 1)

    def test_digest_error_reported(self):
        self.dev.corrupt_put = True
        rc, _, err = self.rpc("put", self.local("x", b"payload" * 50), "/tmp/x")
        self.assertEqual(rc, 1)
        self.assertIn("[digest]", err)
        self.assertNotIn("/tmp/x", self.dev.fs.files)

    def test_fetch_from_a_url(self):
        src = self.local("served file.txt", b"over http")
        url = "file://" + src.replace(os.sep, "/").replace(" ", "%20")
        rc, _, err = self.rpc("fetch", url)
        self.assertEqual(rc, 0, err)
        self.assertEqual(self.dev.fs.files["/served file.txt"], b"over http")
        rc, _, err = self.rpc("fetch", url, "/tmp/")
        self.assertEqual(self.dev.fs.files["/tmp/served file.txt"], b"over http")


class TestInstall(CliCase):
    def setUp(self):
        CliCase.setUp(self)
        self.blobs = {}
        pkgs = []

        def add(name, arch, ver="1.0", sha=True, wrong=False, url=None):
            data = make_app(name, ver, 900)
            fn = "{}-{}.app".format(name, arch.replace(",", "_") or "any")
            self.local(os.path.join("repo", fn), data)
            entry = {"name": name, "ver": ver, "arch": arch, "url": url or fn, "size": len(data),
                     "desc": "test package " + name, "kind": "app"}
            if sha:
                entry["sha256"] = "0" * 64 if wrong else hashlib.sha256(data).hexdigest()
            pkgs.append(entry)
            self.blobs[(name, arch)] = data

        add("hello", "armv6m")
        add("Multi", "armv6m", "1.0")
        add("Multi", "armv8m", "2.0")
        add("bigone", "armv8m")
        add("nohash", "armv6m", sha=False)
        add("tampered", "armv6m", wrong=True)
        pkgs.append({"name": "nourl", "ver": "1"})
        self.index = self.local(os.path.join("repo", "index.json"),
                                json.dumps({"format": 2, "packages": pkgs}).encode())

    def test_local_app(self):
        app = self.local("my pkg.app", make_app("mine", "3.1"))
        rc, out, err = self.rpc("install", app)
        self.assertEqual(rc, 0, err)
        self.assertIn(b"Installed mine 3.1", out)
        self.assertEqual(self.dev.installed["mine"], "3.1")
        self.assertFalse(any(p.startswith("/tmp/") for p in self.dev.fs.files), "temp file left")

    def test_bad_local_app(self):
        app = self.local("junk.app", b"not a package")
        rc, out, _ = self.rpc("install", app)
        self.assertEqual(rc, 1)
        self.assertIn(b"Not a valid package", out)
        self.assertFalse(any(p.startswith("/tmp/") for p in self.dev.fs.files))

    def test_by_name_picks_the_architecture(self):
        rc, out, err = self.rpc("install", "--index", self.index, "multi")
        self.assertEqual(rc, 0, err)
        self.assertEqual(self.dev.installed["Multi"], "2.0")         # armv8m on a Pico 2 W
        self.assertIn("Checksum verified", err)

    def test_rp2040_refuses_armv8m(self):
        self.dev.board = "pico_w"
        rc, out, err = self.rpc("install", "--index", self.index, "multi")
        self.assertEqual(self.dev.installed["Multi"], "1.0")
        rc, out, err = self.rpc("install", "--index", self.index, "bigone")
        self.assertEqual(rc, 1)
        self.assertIn("built for armv8m; this board is armv6m", err)

    def test_unknown_board_needs_arch(self):
        self.dev.board = "esp32s3"
        self.dev.send_arch = False          # firmware that does not say
        rc, out, err = self.rpc("install", "--index", self.index, "hello")
        self.assertEqual(rc, 1)
        self.assertIn("--arch", err)
        self.assertNotIn("hello", self.dev.installed)
        rc, out, err = self.rpc("install", "--index", self.index, "--arch", "armv6m", "hello")
        self.assertEqual(rc, 0, err)

    def test_checksum_mismatch_installs_nothing(self):
        before = dict(self.dev.fs.files)
        rc, out, err = self.rpc("install", "--index", self.index, "tampered")
        self.assertEqual(rc, 1)
        self.assertIn("Checksum mismatch", err)
        self.assertNotIn("tampered", self.dev.installed)
        self.assertEqual(self.dev.fs.files, before)

    def test_unverified_warns(self):
        rc, out, err = self.rpc("install", "--index", self.index, "nohash")
        self.assertEqual(rc, 0)
        self.assertIn("installed unverified", err)

    def test_unknown_name(self):
        rc, out, err = self.rpc("install", "--index", self.index, "nonesuch")
        self.assertEqual(rc, 1)
        self.assertIn("No package called", err)

    def test_list(self):
        rc, out, err = run_cli(None, ["install", "--list", "--index", self.index, "--board", "pico"])
        text = out.decode()
        self.assertEqual(rc, 0, err)
        self.assertIn("hello", text)
        self.assertNotIn("nourl", text)
        self.assertIn("(not for armv6m)", [ln for ln in text.splitlines() if "bigone" in ln][0])

    def test_list_real_fixture(self):
        fixture = os.path.join(support.REPO, "os", "host", "fixtures", "index-v2.json")
        rc, out, err = run_cli(None, ["install", "--list", "--index", fixture])
        self.assertEqual(rc, 0, err)
        self.assertIn("greet", out.decode())


class TestBackup(CliCase):
    def check_roundtrip(self, dest, packages):
        self.dev.fs.dirs.add("/home/root/empty dir")
        self.dev.fs.files["/home/root/" + SNOW + ".txt"] = b"unicode name"
        self.dev.fs.files["/toplevel.txt"] = b"at the top"
        args = ["backup", dest] + (["--with-packages"] if packages else [])
        rc, out, err = self.rpc(*args)
        self.assertEqual(rc, 0, err)
        fresh = MockDevice().start()
        self.addCleanup(fresh.shutdown)
        for p in list(fresh.fs.files):
            if not p.startswith("/os/"):
                del fresh.fs.files[p]
        fresh.installed = {}
        rc, out, err = run_cli(fresh, ["restore", dest], env=self.env)
        self.assertEqual(rc, 0, err)
        self.assertEqual(fresh.auths, ["rootpw"], "restoring /etc and packages asks for sudo once")
        for p in ("/home/root/notes.txt", "/home/root/" + SNOW + ".txt", "/toplevel.txt",
                  "/etc/startup.cfg", "/usb/readme.txt"):
            self.assertEqual(fresh.fs.files[p], self.dev.fs.files[p], p)
        self.assertTrue(fresh.fs.isdir("/home/root/empty dir"))
        self.assertEqual("greet" in fresh.installed, packages)
        return fresh

    def test_directory(self):
        self.check_roundtrip(os.path.join(self.tmp, "bk"), False)
        with open(os.path.join(self.tmp, "bk", "rpc-backup.json")) as f:
            man = json.load(f)
        paths = [e["path"] for e in man["files"]]
        self.assertIn("/home/root/notes.txt", paths)
        self.assertFalse(any(p.startswith("/os/") for p in paths), "system files backed up")
        self.assertEqual(man["device"]["board"], "pico2_w")

    def test_zip_with_packages(self):
        dest = os.path.join(self.tmp, "bk.zip")
        self.check_roundtrip(dest, True)
        names = zipfile.ZipFile(dest).namelist()
        self.assertIn("packages/greet.app", names)
        self.assertNotIn("files/os/registry.cfg", names)

    def test_dry_run(self):
        dest = os.path.join(self.tmp, "bk.zip")
        self.rpc("backup", dest)
        self.dev.fs.remove("/home/root/notes.txt")
        rc, out, err = self.rpc("restore", "--dry-run", dest)
        self.assertEqual(rc, 0)
        self.assertIn("would put /home/root/notes.txt", err)
        self.assertNotIn("/home/root/notes.txt", self.dev.fs.files)

    def test_guest_skips_what_it_cannot_read(self):
        self.dev.user = "guest"
        rc, out, err = self.rpc("backup", os.path.join(self.tmp, "g"))
        self.assertEqual(rc, 1)
        self.assertIn("skipped", err)
        self.assertIn(b"skipped", out)


class TestSudo(CliCase):
    """Installing and changing /os or /etc need sudo on protocol 2 firmware:
    the device answers ERR sudo, rpc sends AUTH with the account's password,
    and the request is made again."""
    env = {}

    def requests(self, prefix):
        return [r for r in self.dev.requests if r.startswith(prefix)]

    def test_install_without_a_password_is_refused_and_says_how(self):
        rc, out, err = self.rpc("install", self.local("a.app", make_app("mine")))
        self.assertEqual(rc, 1)
        self.assertIn("needs sudo", err)
        self.assertIn("RPC_PASSWORD", err)
        self.assertNotIn("mine", self.dev.installed)
        self.assertEqual(self.dev.auths, [])
        self.assertFalse(any(p.startswith("/tmp/") for p in self.dev.fs.files), "temp file left")

    def test_install_with_the_password_in_the_environment(self):
        rc, out, err = run_cli(self.dev, ["install", self.local("a.app", make_app("mine"))],
                               env={"RPC_PASSWORD": "rootpw"})
        self.assertEqual(rc, 0, err)
        self.assertEqual(self.dev.installed["mine"], "1.0")
        self.assertEqual(self.dev.auths, ["rootpw"])
        self.assertTrue(self.requests("EXEC sudo pkg install"), self.dev.requests)

    def test_a_wrong_password_from_the_environment_is_not_tried_again(self):
        rc, out, err = run_cli(self.dev, ["install", self.local("a.app", make_app("mine"))],
                               env={"RPC_PASSWORD": "nope"})
        self.assertEqual(rc, 1)
        self.assertIn("refused the password", err)
        self.assertEqual(self.dev.auths, ["nope"])
        self.assertNotIn("mine", self.dev.installed)

    def test_asked_with_getpass_and_asked_again_when_wrong(self):
        answers = ["wrong", "rootpw"]
        asked = []

        def fake_getpass(prompt):
            asked.append(prompt)
            return answers.pop(0)
        with mock.patch("getpass.getpass", fake_getpass), \
                mock.patch.object(sys.stdin, "isatty", lambda: True):
            rc, out, err = self.rpc("install", self.local("a.app", make_app("mine")))
        self.assertEqual(rc, 0, err)
        self.assertEqual(self.dev.auths, ["wrong", "rootpw"])
        self.assertEqual(len(asked), 2)
        self.assertIn("root", asked[0])
        self.assertIn("sudo", asked[0])

    def test_sudo_flag_authenticates_first(self):
        with mock.patch("getpass.getpass", lambda prompt: "rootpw"), \
                mock.patch.object(sys.stdin, "isatty", lambda: True):
            rc, out, err = self.rpc("--sudo", "exec", "sudo pkg remove greet")
        self.assertEqual(rc, 0, err)
        self.assertEqual(self.dev.requests[0], "AUTH " + "cm9vdHB3")     # base64 of rootpw
        self.assertNotIn("greet", [r for r in self.requests("EXEC") if "ERR" in r])
        self.assertNotIn("greet", self.dev.installed)

    def test_exec_of_a_sudo_line_retries_after_auth(self):
        rc, out, err = run_cli(self.dev, ["exec", "sudo pkg remove greet"],
                               env={"RPC_PASSWORD": "rootpw"})
        self.assertEqual(rc, 0, err)
        self.assertEqual(len(self.requests("EXEC sudo pkg remove greet")), 2,
                         "sent, refused with ERR sudo, AUTH, sent again")
        self.assertNotIn("greet", self.dev.installed)

    def test_exec_without_sudo_is_the_device_saying_so(self):
        rc, out, err = run_cli(self.dev, ["exec", "pkg remove greet"],
                               env={"RPC_PASSWORD": "rootpw"})
        self.assertEqual(rc, 1)
        self.assertIn(b"Run it with sudo: sudo pkg remove greet", out)
        self.assertEqual(self.dev.auths, [], "no password is sent for a line that did not ask for sudo")

    def test_put_and_rm_in_etc(self):
        src = self.local("s.cfg", b"# mine\n")
        env = {"RPC_PASSWORD": "rootpw"}
        rc, out, err = run_cli(self.dev, ["put", src, "/etc/startup.cfg"], env=env)
        self.assertEqual(rc, 0, err)
        self.assertEqual(self.dev.fs.files["/etc/startup.cfg"], b"# mine\n")
        self.assertEqual(self.dev.auths, ["rootpw"])
        rc, out, err = run_cli(self.dev, ["rm", "/etc/startup.cfg"], env=env)
        self.assertEqual(rc, 0, err)
        self.assertNotIn("/etc/startup.cfg", self.dev.fs.files)
        rc, out, err = run_cli(self.dev, ["put", src, "/home/root/s.cfg"], env={})
        self.assertEqual(rc, 0, "a home needs no sudo: " + err)

    def test_reading_the_account_file_asks_for_sudo(self):
        rc, out, err = run_cli(self.dev, ["cat", "/os/users.cfg"], env={})
        self.assertEqual(rc, 1)
        self.assertIn("needs sudo", err)
        rc, out, err = run_cli(self.dev, ["cat", "/os/users.cfg"], env={"RPC_PASSWORD": "rootpw"})
        self.assertEqual(rc, 0, err)
        self.assertEqual(out, self.dev.fs.files["/os/users.cfg"])
        rc, out, err = run_cli(self.dev, ["cat", "/os/registry.cfg"], env={})
        self.assertEqual(rc, 0, "the settings need no sudo to read: " + err)

    def test_a_guest_gets_nothing_from_it(self):
        self.dev.user = "guest"
        rc, out, err = run_cli(self.dev, ["install", self.local("a.app", make_app("mine"))],
                               env={"RPC_PASSWORD": "rootpw"})
        self.assertEqual(rc, 1)
        self.assertIn(b"not an admin", out)
        self.assertNotIn("mine", self.dev.installed)

    def test_older_firmware_is_never_asked(self):
        old = MockDevice(proto=1).start()
        self.addCleanup(old.shutdown)
        rc, out, err = run_cli(old, ["install", self.local("a.app", make_app("mine"))], env={})
        self.assertEqual(rc, 0, err)
        self.assertEqual(old.installed["mine"], "1.0")
        self.assertEqual(old.auths, [])
        self.assertFalse([r for r in old.requests if r.startswith("AUTH") or "sudo" in r],
                         old.requests)
        rc, out, err = run_cli(old, ["--sudo", "put", self.local("x", b"x"), "/etc/x"], env={})
        self.assertEqual(rc, 0, err)
        self.assertIn("no sudo", err)


class TestLegacyCli(CliCase):
    mock_args = {"link": False}

    def test_legacy_commands(self):
        rc, out, err = self.rpc("exec", "echo legacy")
        self.assertEqual((rc, out), (0, b"legacy\n"))
        self.assertIn("legacy", err)
        self.assertEqual(self.rpc("exec", "fail")[0], 1)
        src = self.local("f.txt", b"old firmware")
        self.assertEqual(self.rpc("put", src, "/tmp/f.txt")[0], 0)
        self.assertEqual(self.dev.fs.files["/tmp/f.txt"], b"old firmware")
        rc, out, _ = self.rpc("ls", "/tmp")
        self.assertIn(b"f.txt", out)
        rc, out, _ = self.rpc("cat", "/tmp/f.txt")
        self.assertIn(b"old firmware", out)
        rc, out, err = self.rpc("get", "/tmp/f.txt")
        self.assertEqual(rc, 1)
        self.assertIn("link mode", err)
        rc, out, err = self.rpc("install", self.local("p.app", make_app("oldpkg")))
        self.assertEqual(rc, 0, err)
        self.assertIn("oldpkg", self.dev.installed)
        rc, out, err = self.rpc("info")
        self.assertIn(b"legacy", out)
        index = self.local("idx.json", json.dumps({"packages": [
            {"name": "only8", "arch": "armv8m", "url": "o.app"}]}).encode())
        self.dev.board = "pico_w"           # read from `ver` on this firmware
        rc, out, err = self.rpc("install", "--index", index, "only8")
        self.assertEqual(rc, 1)
        self.assertIn("this board is armv6m", err)


class TestLoginCli(CliCase):
    mock_args = {"login": True}

    def test_login_needed(self):
        rc, out, err = self.rpc("exec", "whoami")
        self.assertEqual(rc, 1)
        self.assertIn("--user", err)
        self.assertEqual(self.dev.login_typed, [])
        rc, out, err = run_cli(self.dev, ["--user", "root", "exec", "whoami"],
                               env={"RPC_PASSWORD": "rootpw"})
        self.assertEqual((rc, out), (0, b"root\n"), err)


class TestPortsAndFlash(CliCase):
    def test_ports(self):
        cands = [transport.PortInfo("/dev/ttyACM9", "x", 0x2E8A, 0x0011, "Nova Labs", "RPCortex", "E66")]
        with support.no_real_ports(), mock.patch.object(transport, "list_candidates", lambda: cands):
            out = support._Out()
            with mock.patch.object(sys, "stdout", out):
                rc = support.cli.main(["ports"])
        self.assertEqual(rc, 0)
        self.assertIn("/dev/ttyACM9", out.text())
        self.assertIn("RPCortex", out.text())
        rc, _, err = run_cli(None, ["ports"])
        self.assertEqual(rc, 1)

    def test_choose_port_prefers_rpcortex_and_env(self):
        a = transport.PortInfo("/dev/a", "", 0x2E8A, 0x000A, "", "Pico", "")
        b = transport.PortInfo("/dev/b", "", 0x2E8A, 0x0011, "Nova Labs", "RPCortex", "")
        with support.no_real_ports(), mock.patch.object(transport, "list_candidates", lambda: [a, b]):
            self.assertEqual(transport.choose_port(None), "/dev/b")
            os.environ["RPC_PORT"] = "/dev/env"
            self.assertEqual(transport.choose_port(None), "/dev/env")
            self.assertEqual(transport.choose_port("/dev/explicit"), "/dev/explicit")
        c = transport.PortInfo("/dev/c", "", 0x2E8A, 0x0011, "Nova Labs", "RPCortex", "")
        with support.no_real_ports(), mock.patch.object(transport, "list_candidates", lambda: [b, c]), \
                mock.patch.object(sys.stdin, "isatty", lambda: False):
            with self.assertRaises(support.cli.RpcError):
                transport.choose_port(None)

    def make_uf2(self, family):
        import struct
        blocks = []
        for i in range(4):
            blk = struct.pack("<8I", 0x0A324655, 0x9E5D5157, 0x2000, 0x10000000 + i * 256, 256, i, 4,
                              family)
            blk += b"\0" * 476 + struct.pack("<I", 0x0AB16F30)
            blocks.append(blk)
        return self.local("fw.uf2", b"".join(blocks))

    def drive(self, chip):
        d = os.path.join(self.tmp, "RP2350" if chip == "RP2350" else "RPI-RP2")
        os.makedirs(d)
        with open(os.path.join(d, "INFO_UF2.TXT"), "w") as f:
            f.write("UF2 Bootloader v1.0\nModel: Raspberry Pi {}\nBoard-ID: {}\n".format(
                chip, "RP2350" if chip == "RP2350" else "RPI-RP2"))
        return d

    def flash(self, argv, drives, touched, which=None, calls=None):
        out, err = support._Out(), support._Out()
        state = {"reset": False}

        def touch(path):
            touched.append(path)
            state["reset"] = True
        with support.no_real_ports(), \
                mock.patch.object(transport, "touch_1200", touch), \
                mock.patch.object(transport, "list_candidates", lambda: [
                    transport.PortInfo("/dev/mockacm", "", 0x2E8A, 0x0011, "Nova Labs", "RPCortex", "")]), \
                mock.patch.object(flash, "find_drives", lambda: drives if state["reset"] or "--no-reset" in argv else []), \
                mock.patch.object(sys, "stdout", out), mock.patch.object(sys, "stderr", err):
            if which:
                with mock.patch("shutil.which", which), mock.patch.object(flash, "_quiet",
                                                                   lambda cmd: calls.append(cmd) or 0), \
                        mock.patch("subprocess.run", side_effect=AssertionError("a test must never run a real command")), \
                        mock.patch("subprocess.call", side_effect=AssertionError("a test must never run a real command")):
                    rc = support.cli.main(argv)
            else:
                rc = support.cli.main(argv)
        return rc, out.text(), err.text()

    def test_flash_copies_to_the_drive(self):
        uf2 = self.make_uf2(0xE48BFF59)
        d = self.drive("RP2350")
        touched = []
        rc, out, err = self.flash(["flash", uf2, "--wait", "1"], [d], touched)
        self.assertEqual(rc, 0, err)
        self.assertEqual(touched, ["/dev/mockacm"])
        with open(os.path.join(d, "fw.uf2"), "rb") as f, open(uf2, "rb") as g:
            self.assertEqual(f.read(), g.read())
        self.assertIn("files on it are kept", out)

    def test_flash_refuses_the_wrong_chip(self):
        uf2 = self.make_uf2(0xE48BFF56)          # an RP2040 image
        d = self.drive("RP2350")
        rc, out, err = self.flash(["flash", uf2, "--wait", "1"], [d], [])
        self.assertEqual(rc, 1)
        self.assertIn("Nothing was written", err)
        self.assertFalse(os.path.exists(os.path.join(d, "fw.uf2")))

    def test_flash_no_drive(self):
        uf2 = self.make_uf2(0xE48BFF59)
        rc, out, err = self.flash(["flash", uf2, "--wait", "0.2"], [], [])
        self.assertEqual(rc, 1)
        self.assertIn("BOOTSEL", err)

    @unittest.skipUnless(sys.platform.startswith("linux"), "picotool fallback is Linux only")
    def test_flash_picotool_fallback(self):
        uf2 = self.make_uf2(0xE48BFF59)
        calls = []
        rc, out, err = self.flash(["flash", uf2, "--wait", "0.2"], [], [],
                                  which=lambda n: "/usr/bin/picotool", calls=calls)
        self.assertEqual(rc, 0, err)
        self.assertEqual(calls, [["/usr/bin/picotool", "load", uf2], ["/usr/bin/picotool", "reboot"]])

    def test_flash_rejects_non_uf2(self):
        bad = self.local("bad.uf2", b"\0" * 1024)
        rc, out, err = self.flash(["flash", bad], [], [])
        self.assertEqual(rc, 1)
        self.assertIn("not a UF2", err)


if __name__ == "__main__":
    unittest.main()
