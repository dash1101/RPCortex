// What it takes to run each command: the three levels, the commands whose level
// depends on their arguments, and the settings that decide who gets in.
//
// The rules are core/perms.cpp, tested directly. The last part reads the
// shell's own sources and checks each command that matters is REGISTERED with
// the level and forms these tests are about: a rule that is right but not
// attached to its command protects nothing, and nothing at run time would say.
#include "perms.h"
#include "users.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>

static int checks = 0, fails = 0;
static void ck(bool c, const char *m) {
    checks++;
    if (!c) { fails++; printf("  FAIL: %s\n", m); }
}

static uint32_t g_seed = 3;
extern "C" uint32_t rpc_rand32(void) { g_seed = g_seed * 1664525u + 1013904223u; return g_seed; }

// The level for a typed line, as run_one asks it: argv split on spaces.
static CmdLevel lv(CmdLevel base, uint8_t forms, const char *line, const char *user = "root") {
    static char buf[256];
    snprintf(buf, sizeof(buf), "%s", line);
    char *argv[16];
    int argc = 0;
    for (char *t = strtok(buf, " "); t && argc < 16; t = strtok(nullptr, " ")) argv[argc++] = t;
    return perms_level(base, forms, argc, argv, user);
}

static std::string label(uint8_t forms, const char *line) {
    static char buf[256];
    snprintf(buf, sizeof(buf), "%s", line);
    char *argv[16];
    int argc = 0;
    for (char *t = strtok(buf, " "); t && argc < 16; t = strtok(nullptr, " ")) argv[argc++] = t;
    char out[48];
    perms_label(forms, argc, argv, out, sizeof(out));
    return out;
}

// The registration of `name` in `file`, from its name to the end of the entry.
static std::string registration(const char *file, const char *name) {
    FILE *f = fopen(file, "rb");
    if (!f) return "";
    std::string s;
    char b[4096];
    size_t n;
    while ((n = fread(b, 1, sizeof(b), f)) > 0) s.append(b, n);
    fclose(f);
    std::string key = std::string("{\"") + name + "\",";
    size_t at = s.find(key);
    if (at == std::string::npos) return "";
    size_t end = s.find('}', at);
    return s.substr(at, end == std::string::npos ? std::string::npos : end - at);
}

static void registered(const char *file, const char *name, const char *level, const char *forms) {
    std::string r = registration(file, name);
    char m[160];
    snprintf(m, sizeof(m), "%s is registered in %s with %s%s%s", name, file, level,
             forms ? " and " : "", forms ? forms : "");
    bool ok = !r.empty() && r.find(level) != std::string::npos &&
              (!forms || r.find(forms) != std::string::npos);
    if (!ok && !r.empty()) printf("    found: %s\n", r.c_str());
    ck(ok, m);
}

int main(void) {
    users_add("root", "pw", true, false);
    users_add("bob", "pw", false, false);
    users_add("guest", nullptr, false, true);

    // --- the three levels ------------------------------------------------------------
    ck(perms_allows("guest", false, LEVEL_USER, false), "anyone may run a user command");
    ck(!perms_allows("bob", false, LEVEL_ADMIN, false) && !perms_allows("bob", false, LEVEL_ADMIN, true),
       "an ordinary account may not run an admin command, with or without sudo");
    ck(perms_allows("root", true, LEVEL_ADMIN, false), "an admin may, without sudo");
    ck(!perms_allows("root", true, LEVEL_SYSTEM, false), "a system command needs sudo, even for root");
    ck(perms_allows("root", true, LEVEL_SYSTEM, true), "and runs with it");
    ck(!perms_allows("bob", false, LEVEL_SYSTEM, true), "sudo grants nothing to an ordinary account");

    // --- pkg -------------------------------------------------------------------------
    for (const char *l : {"pkg", "pkg list", "pkg search", "pkg search gps", "pkg info greet",
                          "pkg update", "pkg repo", "pkg repo list", "pkg certs"})
        ck(lv(LEVEL_ADMIN, FORMS_PKG, l) == LEVEL_ADMIN, l);
    for (const char *l : {"pkg install greet", "pkg install /tmp/x.app", "pkg remove greet",
                          "pkg upgrade", "pkg repo add https://x", "pkg repo remove 1",
                          "pkg certs install", "pkg reinstall greet", "pkg frobnicate"})
        ck(lv(LEVEL_ADMIN, FORMS_PKG, l) == LEVEL_SYSTEM, l);

    // --- update ----------------------------------------------------------------------
    for (const char *l : {"update", "update check", "update rollback", "update revert"})
        ck(lv(LEVEL_ADMIN, FORMS_UPDATE, l) == LEVEL_ADMIN, l);
    for (const char *l : {"update install", "update install --force", "update online",
                          "update from-file /tmp/x.bin", "update rollback --now",
                          "update rollback --save", "update rollback --forget", "update anything"})
        ck(lv(LEVEL_ADMIN, FORMS_UPDATE, l) == LEVEL_SYSTEM, l);

    // --- ssh -------------------------------------------------------------------------
    for (const char *l : {"ssh", "ssh dash@10.0.0.2", "ssh keygen", "ssh pubkey",
                          "ssh server", "ssh server status", "ssh server fingerprint"})
        ck(lv(LEVEL_USER, FORMS_SSH, l, "guest") == LEVEL_USER, l);
    for (const char *l : {"ssh server on", "ssh server off", "ssh server port 2222",
                          "ssh server rekey", "ssh server start"})
        ck(lv(LEVEL_USER, FORMS_SSH, l) == LEVEL_SYSTEM, l);

    // --- what runs later -----------------------------------------------------------------
    ck(lv(LEVEL_ADMIN, FORMS_STARTUP, "startup") == LEVEL_ADMIN &&
       lv(LEVEL_ADMIN, FORMS_STARTUP, "startup list") == LEVEL_ADMIN, "startup list is everyday");
    for (const char *l : {"startup add ls", "startup remove 1", "startup rm 1", "startup clear",
                          "startup run"})
        ck(lv(LEVEL_ADMIN, FORMS_STARTUP, l) == LEVEL_SYSTEM, l);
    ck(lv(LEVEL_ADMIN, FORMS_TASK, "task list") == LEVEL_ADMIN, "task list is everyday");
    for (const char *l : {"task add 60 ls", "task start", "task stop", "task reload", "task clear"})
        ck(lv(LEVEL_ADMIN, FORMS_TASK, l) == LEVEL_SYSTEM, l);
    for (const char *l : {"service", "service list", "service status"})
        ck(lv(LEVEL_ADMIN, FORMS_SERVICE, l) == LEVEL_ADMIN, l);
    for (const char *l : {"service add ls", "service remove 1", "service start", "service clear"})
        ck(lv(LEVEL_ADMIN, FORMS_SERVICE, l) == LEVEL_SYSTEM, l);
    ck(lv(LEVEL_ADMIN, FORMS_AUTONOMY, "autonomy") == LEVEL_ADMIN &&
       lv(LEVEL_ADMIN, FORMS_AUTONOMY, "autonomy status") == LEVEL_ADMIN, "autonomy status is everyday");
    ck(lv(LEVEL_ADMIN, FORMS_AUTONOMY, "autonomy on") == LEVEL_SYSTEM &&
       lv(LEVEL_ADMIN, FORMS_AUTONOMY, "autonomy on root") == LEVEL_SYSTEM &&
       lv(LEVEL_ADMIN, FORMS_AUTONOMY, "autonomy off") == LEVEL_SYSTEM, "turning autonomy on or off needs sudo");
    ck(lv(LEVEL_ADMIN, FORMS_SAFEBOOT, "safeboot") == LEVEL_ADMIN,
       "a plain safeboot is an admin's (it is the way back from a forgotten password)");
    ck(lv(LEVEL_ADMIN, FORMS_SAFEBOOT, "safeboot script /x.rps") == LEVEL_SYSTEM,
       "a staged command runs with sudo, so staging one needs it");

    // --- accounts --------------------------------------------------------------------------
    ck(lv(LEVEL_ADMIN, FORMS_MKACCT, "mkacct sam") == LEVEL_ADMIN &&
       lv(LEVEL_ADMIN, FORMS_MKACCT, "mkacct") == LEVEL_ADMIN, "an ordinary account is an admin's to make");
    ck(lv(LEVEL_ADMIN, FORMS_MKACCT, "mkacct sam --admin") == LEVEL_SYSTEM &&
       lv(LEVEL_ADMIN, FORMS_MKACCT, "mkacct sam --nopass --admin") == LEVEL_SYSTEM &&
       lv(LEVEL_ADMIN, FORMS_MKACCT, "mkacct --admin") == LEVEL_SYSTEM, "an admin account needs sudo");
    ck(lv(LEVEL_USER, FORMS_PASSWD, "passwd", "bob") == LEVEL_USER &&
       lv(LEVEL_USER, FORMS_PASSWD, "passwd bob", "bob") == LEVEL_USER &&
       lv(LEVEL_USER, FORMS_PASSWD, "passwd", "root") == LEVEL_USER,
       "your own password is yours to change");
    ck(lv(LEVEL_USER, FORMS_PASSWD, "passwd", "guest") == LEVEL_ADMIN,
       "but an account with none cannot give itself one (that would open it to SSH)");
    ck(lv(LEVEL_USER, FORMS_PASSWD, "passwd root", "bob") == LEVEL_SYSTEM &&
       lv(LEVEL_USER, FORMS_PASSWD, "passwd bob", "root") == LEVEL_SYSTEM,
       "somebody else's needs sudo");

    // --- the rest ------------------------------------------------------------------------------
    ck(lv(LEVEL_USER, FORMS_STOCK, "stock list") == LEVEL_USER, "stock list is anyone's");
    ck(lv(LEVEL_USER, FORMS_STOCK, "stock") == LEVEL_SYSTEM &&
       lv(LEVEL_USER, FORMS_STOCK, "stock greet") == LEVEL_SYSTEM, "installing a built-in package needs sudo");
    ck(lv(LEVEL_ADMIN, FORMS_REG, "reg") == LEVEL_ADMIN &&
       lv(LEVEL_ADMIN, FORMS_REG, "reg get System.Autonomous") == LEVEL_ADMIN &&
       lv(LEVEL_ADMIN, FORMS_REG, "reg set System.Owner Dash") == LEVEL_ADMIN,
       "reading settings, and ordinary ones, are an admin's");
    for (const char *l : {"reg set System.Autonomous root", "reg set System.Setup false",
                          "reg set System.SafeBoot true", "reg set System.SafeBootCmd x",
                          "reg set SSH.Server on", "reg set Alias.ls rm", "reg set Pkg.Repos https://x"})
        ck(lv(LEVEL_ADMIN, FORMS_REG, l) == LEVEL_SYSTEM, l);
    perms_set_packages_privileged(false);
    ck(lv(LEVEL_USER, FORMS_RUN, "run /home/root/x.app") == LEVEL_USER,
       "with a sandbox, running a package is anyone's");
    ck(perms_forms_text(FORMS_RUN) == nullptr, "and sudo -l does not list it");
    perms_set_packages_privileged(true);
    ck(lv(LEVEL_USER, FORMS_RUN, "run /home/root/x.app") == LEVEL_SYSTEM &&
       lv(LEVEL_USER, FORMS_RUN, "run /home/root/x") == LEVEL_SYSTEM,
       "without one, a package is the whole device: sudo");
    ck(lv(LEVEL_USER, FORMS_RUN, "run /home/root/x.RPS") == LEVEL_USER, "a script is lines, each judged on its own");
    ck(perms_forms_text(FORMS_RUN) != nullptr, "and sudo -l lists it there");
    perms_set_packages_privileged(false);
    ck(lv(LEVEL_SYSTEM, FORMS_NONE, "rmuser bob") == LEVEL_SYSTEM, "a system command is that for every form");
    ck(lv(LEVEL_ADMIN, FORMS_NONE, "reboot") == LEVEL_ADMIN && lv(LEVEL_USER, FORMS_NONE, "ls /") == LEVEL_USER,
       "and the others stay where they were");
    ck(lv(LEVEL_ADMIN, 200, "pkg install x") == LEVEL_ADMIN, "a forms number past the table changes nothing");

    // --- the name in a message --------------------------------------------------------------------
    ck(label(FORMS_PKG, "pkg install greet") == "pkg install", "pkg install is named as such");
    ck(label(FORMS_SSH, "ssh server on") == "ssh server on", "ssh server on is three words");
    ck(label(FORMS_UPDATE, "update --force") == "update", "an option is not part of a name");
    ck(label(FORMS_NONE, "rmuser bob") == "rmuser", "a whole-command one is its name");
    ck(label(FORMS_MKACCT, "mkacct sam --admin") == "mkacct", "an operand is not part of a name");

    // --- settings ----------------------------------------------------------------------------------
    for (const char *k : {"System.Autonomous", "System.Setup", "System.Active_User", "System.SafeBoot",
                          "System.SafeBootCmd", "SSH.Server", "SSH.Port", "Alias.ll", "Pkg.Repos"})
        ck(perms_reg_key_system(k), k);
    for (const char *k : {"WiFi.Auto", "System.Owner", "Apps.NovaD1.Pins", "User.Theme", "Pkg.Disabled"})
        ck(!perms_reg_key_system(k), k);
    ck(!perms_reg_write_ok("SSH.Server", false, false, false) &&
       !perms_reg_write_ok("System.SafeBootCmd", false, false, false),
       "an admin's package, without sudo, cannot write the keys that decide who gets in");
    ck(perms_reg_write_ok("WiFi.Auto", false, false, false), "but writes every other");
    ck(perms_reg_write_ok("SSH.Server", false, false, true), "with sudo, anything");
    ck(!perms_reg_write_ok("System.SafeBoot", true, false, true) &&
       !perms_reg_write_ok("Pkg.Repos", true, false, false),
       "an ordinary account's package never (it could stage a command for the next start)");
    ck(perms_reg_write_ok("Apps.Score", true, true, false) && !perms_reg_write_ok("WiFi.Auto", true, true, false),
       "a remote ordinary account's package only its own");

    // --- and each rule is attached to its command --------------------------------------------------
    registered("../shell/pkg.cpp", "pkg", "LEVEL_ADMIN", "FORMS_PKG");
    registered("../shell/update.cpp", "update", "LEVEL_ADMIN", "FORMS_UPDATE");
    registered("../shell/sshc.cpp", "ssh", "LEVEL_USER", "FORMS_SSH");
    registered("../shell/jobs.cpp", "startup", "LEVEL_ADMIN", "FORMS_STARTUP");
    registered("../shell/jobs.cpp", "task", "LEVEL_ADMIN", "FORMS_TASK");
    registered("../shell/jobs.cpp", "service", "LEVEL_ADMIN", "FORMS_SERVICE");
    registered("../shell/session.cpp", "autonomy", "LEVEL_ADMIN", "FORMS_AUTONOMY");
    registered("../shell/user.cpp", "mkacct", "LEVEL_ADMIN", "FORMS_MKACCT");
    registered("../shell/user.cpp", "passwd", "LEVEL_USER", "FORMS_PASSWD");
    registered("../shell/user.cpp", "usermod", "LEVEL_SYSTEM", nullptr);
    registered("../shell/user.cpp", "rmuser", "LEVEL_SYSTEM", nullptr);
    registered("../shell/diag.cpp", "regreset", "LEVEL_SYSTEM", nullptr);
    registered("../shell/diag.cpp", "factoryreset", "LEVEL_SYSTEM", nullptr);
    registered("../shell/diag.cpp", "failboot", "LEVEL_SYSTEM", nullptr);
    registered("../shell/diag.cpp", "safeboot", "LEVEL_ADMIN", "FORMS_SAFEBOOT");
    registered("../shell/stock.cpp", "stock", "LEVEL_USER", "FORMS_STOCK");
    registered("../shell/shell.cpp", "reg", "LEVEL_ADMIN", "FORMS_REG");
    registered("../shell/shell.cpp", "run", "LEVEL_USER", "FORMS_RUN");

    printf("  perms: %d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
