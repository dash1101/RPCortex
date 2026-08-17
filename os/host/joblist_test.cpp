// Line lists: counting, indexing, add and remove.
//
// The bug this exists to prevent is "remove 3 deleted the wrong one", which is
// silent — the user sees a list one shorter and assumes it worked. Comments and
// blank lines are the reason it happens: they are in the file but not in the
// numbering the user is shown.

#include "joblist.h"
#include <stdio.h>
#include <string.h>

static int checks = 0, fails = 0;
static void ck(bool c, const char *m) {
    checks++; if (!c) { fails++; printf("  FAIL: %s\n", m); }
}
static void eq(const char *got, const char *want, const char *what) {
    checks++;
    if (got && strcmp(got, want) == 0) return;
    fails++;
    printf("  FAIL: %s\n    got  |%s|\n    want |%s|\n", what, got ? got : "(null)", want);
}

static char g_seen[512];
static void collect(void *, uint32_t i, const char *line) {
    char b[160];
    snprintf(b, sizeof(b), "%u:%s;", (unsigned)i, line);
    strncat(g_seen, b, sizeof(g_seen) - strlen(g_seen) - 1);
}
static const char *walk(const char *buf) {
    g_seen[0] = 0;
    joblist_walk(buf, (uint32_t)strlen(buf), collect, nullptr);
    return g_seen;
}

int main(void) {
    // --- counting, with the things that must NOT count ----------------------
    const char *mixed = "# a header\n\nsysinfo\n   \nwifi scan\n# trailing note\nuptime\n";
    ck(joblist_count(mixed, (uint32_t)strlen(mixed)) == 3, "comments and blanks are not counted");
    eq(walk(mixed), "1:sysinfo;2:wifi scan;3:uptime;", "numbering skips comments and blanks");

    ck(joblist_count("", 0) == 0, "an empty list counts zero");
    ck(joblist_count("# only a comment\n", 16) == 0, "a comment-only file counts zero");

    // --- get by index -------------------------------------------------------
    char out[64];
    ck(joblist_get(mixed, (uint32_t)strlen(mixed), 2, out, sizeof(out)), "get finds entry 2");
    eq(out, "wifi scan", "and it is the right one, not the second raw line");
    ck(!joblist_get(mixed, (uint32_t)strlen(mixed), 0, out, sizeof(out)), "index 0 does not exist");
    ck(!joblist_get(mixed, (uint32_t)strlen(mixed), 4, out, sizeof(out)), "past the end does not exist");

    // --- add ----------------------------------------------------------------
    char buf[512];
    strcpy(buf, "# header\n");
    uint32_t len = (uint32_t)strlen(buf);
    len = joblist_add(buf, len, sizeof(buf), "first");
    len = joblist_add(buf, len, sizeof(buf), "second");
    eq(walk(buf), "1:first;2:second;", "added lines appear in order");
    ck(buf[len] == 0, "the buffer stays terminated");

    // Adding to a file with no trailing newline must not join two lines.
    char nonl[64] = "alpha";
    uint32_t nl = joblist_add(nonl, 5, sizeof(nonl), "beta");
    eq(walk(nonl), "1:alpha;2:beta;", "a missing trailing newline is added first");
    (void)nl;

    len = joblist_add(buf, len, sizeof(buf), "");
    eq(walk(buf), "1:first;2:second;", "an empty line is refused");

    // --- remove, the part that goes wrong -----------------------------------
    strcpy(buf, "# header\nalpha\n\nbeta\n# note\ngamma\n");
    len = (uint32_t)strlen(buf);

    uint32_t after = joblist_remove(buf, len, 2);       // beta
    eq(walk(buf), "1:alpha;2:gamma;", "removing entry 2 removes beta, not a comment");
    ck(strstr(buf, "# header") != nullptr, "the header comment survives");
    ck(strstr(buf, "# note") != nullptr, "the inline comment survives");
    ck(strstr(buf, "beta") == nullptr, "beta is really gone");
    len = after;

    len = joblist_remove(buf, len, 1);
    eq(walk(buf), "1:gamma;", "removing the first entry renumbers the rest");

    len = joblist_remove(buf, len, 9);
    eq(walk(buf), "1:gamma;", "an out-of-range remove changes nothing");

    len = joblist_remove(buf, len, 1);
    eq(walk(buf), "", "the last entry can be removed");
    ck(joblist_count(buf, len) == 0, "and the list is then empty");

    // Removing must not leave a blank line that shifts nothing.
    strcpy(buf, "one\ntwo\nthree\n");
    len = joblist_remove(buf, (uint32_t)strlen(buf), 2);
    eq(buf, "one\nthree\n", "no blank line is left behind");

    // --- interval parsing ---------------------------------------------------
    uint32_t secs = 0; const char *cmd = nullptr;
    ck(joblist_split_interval("60 sysinfo", &secs, &cmd), "a valid interval line parses");
    ck(secs == 60, "the interval is read");
    eq(cmd, "sysinfo", "and the command is what follows");

    ck(joblist_split_interval("  5   wifi scan ", &secs, &cmd), "leading space is fine");
    ck(secs == 5, "interval past the spaces");
    eq(cmd, "wifi scan ", "the whole rest of the line is the command");

    ck(!joblist_split_interval("sysinfo", &secs, &cmd), "no interval is refused");
    ck(!joblist_split_interval("60", &secs, &cmd), "an interval with no command is refused");
    ck(!joblist_split_interval("", &secs, &cmd), "an empty line is refused");
    ck(!joblist_split_interval("99999999 x", &secs, &cmd), "an absurd interval is refused");

    // --- which command a service line runs -----------------------------------
    //
    // `pkg install` reads this to decide whether a running service belongs to
    // the package it is about to replace, and stops it if it does. So the
    // answer picks a task to end — which is why the failure cases below matter
    // more than the ordinary one.
    char w[8];
    ck(joblist_first_word("novad1 gui --bg", w, sizeof(w)), "a service line has a command");
    eq(w, "novad1", "and it is the first word");

    ck(joblist_first_word("  \t httpd", w, sizeof(w)), "leading space and tabs are skipped");
    eq(w, "httpd", "leaving the command");

    ck(joblist_first_word("httpd\tport 80", w, sizeof(w)), "a tab separates too");
    eq(w, "httpd", "and does not end up in the word");

    // A NAME THAT DOES NOT FIT IS REFUSED, NOT SHORTENED. Truncating turns one
    // package's service into another's: "novad1-legacy" cut to seven characters
    // is "novad1", which resolves, matches, and gets stopped on behalf of a
    // package that has nothing to do with it.
    ck(!joblist_first_word("novad1-legacy start", w, sizeof(w)),
       "a command name too long for the buffer is refused");
    eq(w, "", "and nothing shorter is offered in its place");

    ck(!joblist_first_word("", w, sizeof(w)), "an empty line runs nothing");
    ck(!joblist_first_word("   \t ", w, sizeof(w)), "nor does one that is only whitespace");
    ck(!joblist_first_word(nullptr, w, sizeof(w)), "and a null line is not read");

    printf("  joblist: %d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
