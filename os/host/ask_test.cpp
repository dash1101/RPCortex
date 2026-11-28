// apps/browse/ask.cpp on the host: the JSON it reads back (thinking first, several text
// blocks, errors, refusals, escapes, nested blocks it must step over), and the requests it
// builds (headers, no sampling fields, escaping, a conversation that stays user/assistant
// alternating when old turns are dropped).
#include "../include/rpc_app.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <map>

static int checks, fails;
static void ck(bool c, const char *what) { checks++; if (!c) { fails++; printf("  FAIL: %s\n", what); } }

// --- the firmware, faked ---------------------------------------------------------------
static std::map<std::string, std::string> g_reg, g_files;
static std::string g_last_body, g_last_headers, g_reply = "{}";
static int g_reply_status = 200;

extern "C" {
int fw_printf(const char *, ...) { return 0; }
int rpc_register_command(const char *, const char *, RpcCommandFn) { return 1; }
int fw_reg_get(const char *k, char *out, uint32_t cap) {
    auto it = g_reg.find(k); if (it == g_reg.end()) return 0;
    snprintf(out, cap, "%s", it->second.c_str()); return 1;
}
int fw_reg_set(const char *k, const char *v) { g_reg[k] = v; return 1; }
void fw_reg_save(void) {}
uint32_t fw_file_read(const char *p, void *buf, uint32_t cap) {
    auto it = g_files.find(p); if (it == g_files.end()) return 0;
    uint32_t n = (uint32_t)it->second.size() < cap ? (uint32_t)it->second.size() : cap;
    memcpy(buf, it->second.data(), n); return n;
}
int fw_file_write(const char *p, const void *d, uint32_t n) { g_files[p] = std::string((const char *)d, n); return 1; }
int fw_file_remove(const char *p) { g_files.erase(p); return 1; }
void *fw_malloc(size_t n) { return malloc(n); }
void fw_free(void *p) { free(p); }
int fw_line_read(char *, int) { return 0; }
int fw_task_should_stop(void) { return 0; }
int fw_http_request(FwHttpRequest *r) {
    g_last_body.assign((const char *)r->body, r->body_len);
    g_last_headers = r->headers ? r->headers : "";
    g_files[r->path] = g_reply;
    r->status = g_reply_status;
    r->bytes = (unsigned)g_reply.size();
    return (int)g_reply.size();
}
}

#include "../apps/browse/ask.cpp"

static int count(const std::string &s, const char *sub) {
    int n = 0; for (size_t p = s.find(sub); p != std::string::npos; p = s.find(sub, p + 1)) n++;
    return n;
}

int main(void) {
    char out[2048], stop[24];

    // --- reading answers
    ck(parse_answer("{\"content\":[{\"type\":\"thinking\",\"thinking\":\"\",\"signature\":\"abc\"},"
                    "{\"type\":\"text\",\"text\":\"Paris.\"}],\"stop_reason\":\"end_turn\"}",
                    out, sizeof(out), stop, sizeof(stop)) == 1 && !strcmp(out, "Paris.") && !strcmp(stop, "end_turn"),
       "a thinking block first, then the text");
    ck(parse_answer("{\"content\":[{\"type\":\"text\",\"text\":\"one\"},{\"type\":\"text\",\"text\":\"two\"}]}",
                    out, sizeof(out), stop, sizeof(stop)) == 1 && !strcmp(out, "one\ntwo"),
       "two text blocks, joined");
    ck(parse_answer("{\"id\":\"x\",\"content\":[{\"type\":\"tool_use\",\"input\":{\"a\":[1,2,{\"b\":\"}]\\\"\"}],\"n\":null}},"
                    "{\"type\":\"text\",\"text\":\"after\"}],\"usage\":{\"input_tokens\":3}}",
                    out, sizeof(out), stop, sizeof(stop)) == 1 && !strcmp(out, "after"),
       "a block with nested objects and brackets inside strings is stepped over");
    ck(parse_answer("{\"content\":[{\"type\":\"text\",\"text\":\"caf\\u00e9 \\u20ac \\ud83d\\ude00 \\\"q\\\" a\\\\b\\nline\"}]}",
                    out, sizeof(out), stop, sizeof(stop)) == 1 &&
       !strcmp(out, "caf\xc3\xa9 \xe2\x82\xac \xf0\x9f\x98\x80 \"q\" a\\b\nline"),
       "escapes: \\u, a surrogate pair, quotes, backslash, newline");
    ck(parse_answer("{\"type\":\"error\",\"error\":{\"type\":\"rate_limit_error\",\"message\":\"slow down\"}}",
                    out, sizeof(out), stop, sizeof(stop)) == 0 && !strcmp(out, "slow down"),
       "an error gives its message");
    ck(parse_answer("{\"content\":[],\"stop_reason\":\"refusal\"}", out, sizeof(out), stop, sizeof(stop)) == 1 &&
       !strcmp(stop, "refusal"), "a refusal's stop reason is read");
    ck(parse_answer("<html>gateway</html>", out, sizeof(out), stop, sizeof(stop)) == -1, "not JSON: refused");
    ck(parse_answer("{\"content\":[{\"type\":\"text\",\"text\":\"unterminated", out, sizeof(out), stop, sizeof(stop)) != 1 ||
       !out[0] || true, "a truncated body does not crash");

    // --- building requests
    g_reg["System.Active_User"] = "dash";
    g_files["/home/dash/.ask_key"] = "sk-test\n";
    g_reply = "{\"content\":[{\"type\":\"text\",\"text\":\"ok\"}],\"stop_reason\":\"end_turn\"}";
    static char ans[8192];
    hist_clear();
    ck(ask_once("say \"hi\"\n\tplease \\", true, ans, sizeof(ans)) == 1 && !strcmp(ans, "ok"), "one turn answered");
    ck(g_last_headers == "x-api-key: sk-test\nanthropic-version: 2026-10-21\n", "headers: the key (trimmed) and the version");
    ck(g_last_body.find("\"model\":\"claude-opus-5-5\"") != std::string::npos, "the default model");
    ck(g_last_body.find("temperature") == std::string::npos && g_last_body.find("top_p") == std::string::npos,
       "no sampling fields (API 2026-10-21 refuses them)");
    ck(g_last_body.find("say \\\"hi\\\"\\n\\tplease \\\\") != std::string::npos, "the question is escaped");
    ck(ask_once("second", true, ans, sizeof(ans)) == 1, "a second turn");
    ck(count(g_last_body, "\"role\":\"user\"") == 2 && count(g_last_body, "\"role\":\"assistant\"") == 1,
       "the second request carries the first turn and its answer");
    // Fill past the history limit: the oldest PAIRS go, so it still starts with a user turn.
    std::string big(1500, 'x');
    for (int i = 0; i < 12; i++) ask_once(big.c_str(), true, ans, sizeof(ans));
    size_t first = g_last_body.find("\"messages\":[{\"role\":\"");
    ck(first != std::string::npos && g_last_body.compare(first + 21, 4, "user") == 0,
       "after old turns are dropped the conversation still starts with the user");
    ck(count(g_last_body, "\"role\":\"user\"") == count(g_last_body, "\"role\":\"assistant\"") + 1,
       "users and answers stay paired");
    ck(g_hist_len <= ASK_HIST_MAX, "history within its limit");
    g_files.erase("/home/dash/.ask_key");
    ck(ask_once("x", false, ans, sizeof(ans)) == 0 && strstr(ans, "ask key"), "no key: says how to set one");

    printf("  ask: %d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
