// Parsing link-mode requests (see LINK.md). Pure, host-tested.
//
// A request is one line: a verb, then fields separated by single spaces, the
// last of which may be "the rest of the line" (a path or a command, spaces
// included). Nothing here touches the filesystem or the console.
#ifndef RPC_LINKREQ_H
#define RPC_LINKREQ_H

#include <stdint.h>

#define LINK_PROTO      2        // 2: AUTH, and ERR sudo
#define LINK_LINE_MAX   1100     // request line, newline included
#define LINK_CHUNK      768      // bytes per D line or frame (1024 base64 chars)

struct LinkReq {
    char        verb[8];         // upper-case as sent; "" for an empty line
    const char *rest;            // after the verb and one space; "" if none
};

// Split `line` (no newline) into the verb and the rest. False if the verb is
// longer than seven characters or the line is empty.
bool link_parse(const char *line, LinkReq *out);

// PUT's fields: "<size> <sha256 hex> <path...>". False if any is missing or
// malformed (size not a plain decimal, digest not 64 hex characters, no path).
bool link_parse_put(const char *rest, uint32_t *size, char sha_hex[65], const char **path);

// Lower-case a 64-character hex digest in place; false if it is not one.
bool link_norm_hex(char *hex);

#endif
