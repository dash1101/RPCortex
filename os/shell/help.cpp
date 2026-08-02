// help — the front door, in v1's shape.
//
// v1's help was a hand-written category index rather than a dump of the command
// table, and that is deliberate: an alphabetical list of sixty names tells a
// newcomer nothing, while "Filesystem : ls cd pwd ..." tells them where to look.
// The categories here are v1's, minus the ones v2 has no commands for yet — a
// help entry for a command that does not exist is worse than no entry.
//
// `help <category>` gives the detail page. `help all` falls back to the live
// registry, which is the only listing that also shows commands a loaded package
// registered, since those cannot be known at compile time.

#include "command.h"
#include "out.h"

#include <stdio.h>
#include <string.h>

struct Category {
    const char *name;      // what you type after `help`
    const char *title;     // the heading on its detail page
    const char *summary;   // the one-line index entry
    const char *const *lines;
    unsigned    n_lines;
};

static const char *kFilesystem[] = {
    "  ls  [path]           List directory contents",
    "  cd  [path]           Change directory (no arg = root)",
    "  pwd                  Print working directory",
    "  touch <file>         Create an empty file",
    "  mkdir <dir>          Create a directory",
    "  rm  <path>           Delete a file or empty directory",
    "  cat <file>           Print file contents",
    "  head <f> [n]         First n lines  (default 10)",
    "  tail <f> [n]         Last n lines   (default 10)",
    "  rename <old> <new>   Rename in place",
    "  mv  <src> <dst>      Move    (relative or absolute)",
    "  cp  <src> <dst>      Copy    (streamed; relative or absolute)",
    "  df                   Disk usage (whole filesystem)",
    "  du [path]            Size of a file or directory tree",
    "  tree [path]          Directory tree",
    "  put <name> <len>     Upload raw bytes over serial",
};

static const char *kText[] = {
    "  grep <pattern> <file>   Search a file for a pattern (substring)",
    "  wc <file>               Line / word / byte count",
    "  find <name> [path]      Recursive file search by name",
    "  sort <file>             Print lines sorted alphabetically",
    "  uniq <file>             Remove consecutive duplicate lines",
    "  hex <file> [n]          Hexdump the first n bytes  (default 256)",
    "  basename <path>         File name portion of a path",
    "  dirname <path>          Directory portion of a path",
    "  echo <text>             Print text",
};

static const char *kSystem[] = {
    "  sysinfo              System overview",
    "  meminfo / free       RAM usage and fragmentation",
    "  uptime               Time since boot",
    "  date [set ...]       Show date/time, or 'date set YYYY-MM-DD HH:MM:SS'",
    "  ver / uname          Show OS version",
    "  reboot / sreboot     Restart the device",
    "  sleep <secs>         Pause for the given number of seconds",
    "  which <cmd>          Show where a command is defined",
    "  clear / cls          Clear the screen",
    "  pulse set|boot       CPU clock management",
    "  freeup / gc          Report reclaimable memory",
    "  env [section]        Registry settings, grouped",
    "  reg get|set          Read or write a single registry key",
};

static const char *kNetwork[] = {
    "  wifi                    Show the connection status",
    "  wifi scan               List nearby networks",
    "  wifi connect <ssid> [pw]  Join a network and save it",
    "  wifi disconnect         Drop the connection",
    "  wifi list               Saved networks",
    "  wifi forget <ssid>      Remove a saved network",
    "  wifi auto on|off        Reconnect to a saved network at boot",
};

static const char *kPackages[] = {
    "  pkg install <file>   Validate and install a package",
    "  pkg remove <name>    Remove an installed package",
    "  pkg list             Installed packages",
    "  apps                 Packages resident in RAM right now",
    "  unload <name>        Unload a resident package",
    "  run <app> [arg]      Load and run an app without installing it",
};

static const char *kUsers[] = {
    "  whoami / id             The logged-in account",
    "  users                   List accounts",
    "  mkacct <name> [--admin] [--nopass]   Create an account",
    "  passwd [user]           Change a password",
    "  usermod <user> passwd | admin on|off | nopass on|off",
    "  rmuser <username>       Remove an account",
    "  logout / exit           Return to the login prompt",
};

static const char *kMisc[] = {
    "  help [category]      This index, or a category's detail",
    "  help all             Every registered command, including package ones",
    "  history              Recent commands  (up/down arrows recall them)",
};

static const char *kShell[] = {
    "  a ; b                Run b regardless of how a finished",
    "  a && b               Run b only if a succeeded",
    "  a || b               Run b only if a failed",
    "  a | b                Send a's output into b",
    "  a > file             Write a's output to a file  (>> appends)",
    "  \"two words\"          Quote an argument containing spaces",
    "",
    "  Only the data channel is piped or redirected — status lines like",
    "  [@] and [!] still reach the console, so an error during 'ls > f'",
    "  is not silently written into f.",
};

#define CAT(id, name, title, summary) \
    {name, title, summary, id, (unsigned)(sizeof(id) / sizeof(id[0]))}

static const Category kCategories[] = {
    CAT(kFilesystem, "filesystem", "Filesystem Commands",
        "ls  cd  pwd  touch  mkdir  rm  cat  head  tail  rename  mv  cp  df  du  tree  put"),
    CAT(kText, "text", "Text Processing Commands",
        "grep  wc  find  sort  uniq  hex  basename  dirname  echo"),
    CAT(kSystem, "system", "System Commands",
        "sysinfo  meminfo  uptime  date  ver  reboot  sreboot  sleep  which  clear  pulse  freeup  env  reg"),
    CAT(kNetwork, "network", "Network Commands",
        "wifi  (scan  connect  disconnect  list  forget  auto)"),
    CAT(kPackages, "packages", "Packages",
        "pkg install|remove|list   apps  unload  run"),
    CAT(kUsers, "users", "User Accounts",
        "whoami  users  mkacct  passwd  usermod  rmuser  logout"),
    CAT(kMisc, "misc", "Misc",
        "help  history"),
    CAT(kShell, "shell", "Shell Syntax",
        "pipes |   chaining && ||   sequencing ;   redirect > >>"),
};
#define N_CATEGORIES (sizeof(kCategories) / sizeof(kCategories[0]))

// The index labels are padded to a fixed column so the colons line up. The
// category name is capitalised for display from its lowercase key.
static void print_index_line(const Category *c) {
    char label[16];
    snprintf(label, sizeof(label), "%s", c->name);
    if (label[0] >= 'a' && label[0] <= 'z') label[0] = (char)(label[0] - 32);
    out_multi("  %s%-11s%s: %s", C_CYAN, label, C_RESET, c->summary);
}

// help all — the live registry, the only listing that includes commands a
// loaded package registered at runtime.
static int help_all(void) {
    out_info("%u commands registered:", (unsigned)cmd_count());
    for (uint32_t i = 0; i < cmd_count(); i++) {
        const Command *c = cmd_at(i);
        out_multi("  %s%-10s%s %s%s", C_CYAN, c->name, C_RESET, c->help,
                  c->owner ? "  [pkg]" : "");
    }
    if (cmd_alias_count()) {
        out_blank();
        out_info("Aliases:");
        for (uint32_t i = 0; i < cmd_alias_count(); i++) {
            const Alias *a = cmd_alias_at(i);
            out_multi("  %s%-10s%s -> %s", C_GRAY, a->name, C_RESET, a->target);
        }
    }
    return 0;
}

static int cmd_help(int argc, char **argv) {
    if (argc < 2) {
        out_info("=== RPCortex Vela II — Launchpad ===");
        for (unsigned i = 0; i < N_CATEGORIES; i++) print_index_line(&kCategories[i]);
        out_blank();
        out_multi("  Type 'help <category>' for details, or 'help all' for every command.");
        out_multi("  Categories: filesystem  text  system  network  packages  users  misc  shell");
        return 0;
    }

    if (!strcmp(argv[1], "all")) return help_all();

    for (unsigned i = 0; i < N_CATEGORIES; i++) {
        const Category *c = &kCategories[i];
        if (strcmp(argv[1], c->name) != 0) continue;
        out_info("=== %s ===", c->title);
        for (unsigned j = 0; j < c->n_lines; j++) out_multi("%s", c->lines[j]);
        return 0;
    }

    // Not a category — maybe they meant a command. Answering with that command's
    // one-line help is more useful than repeating the index at them.
    const Command *cmd = cmd_resolve(argv[1]);
    if (cmd) {
        out_multi("  %s%s%s  —  %s", C_CYAN, cmd->name, C_RESET, cmd->help);
        return 0;
    }

    out_warn("No help for '%s'. Try 'help' for the index.", argv[1]);
    return 1;
}

void help_register(void) {
    static const Command c{"help", "this index, or help <category>", cmd_help, nullptr};
    cmd_register(&c);
    cmd_alias("man", "help");
    cmd_alias("?",   "help");
}
