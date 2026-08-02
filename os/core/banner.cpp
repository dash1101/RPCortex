// The boot splash — a faithful port of v1's main.py banner.
//
// The same seven-line RPC logo, the same seven-colour gradient cycled per line,
// the same centred "RPCortex <ver> - <codename>" in grey under a 41-character
// rule. This is the first thing anyone sees and it is most of why the OS feels
// like itself, so it is reproduced rather than reinterpreted.

#include "banner.h"
#include "out.h"
#include "kernel.h"

#include <stdio.h>
#include <string.h>

static const char *kLogo[] = {
    "      :::::::::  :::::::::   ::::::::::",
    "     :+:    :+: :+:    :+: :+:    :+: ",
    "    +:+    +:+ +:+    +:+ +:+         ",
    "   +#++:++#:  +#++:++#+  +#+          ",
    "  +#+    +#+ +#+        +#+           ",
    " #+#    #+# #+#        #+#    #+#     ",
    "###    ### ###         ########       ",
};

// v1's gradient, in order: bright cyan, cyan, cyan, bright blue, blue,
// bright magenta, magenta.
static const char *kGradient[] = {
    "\033[96m", "\033[36m", "\033[36m", "\033[94m",
    "\033[34m", "\033[95m", "\033[35m",
};

#define BANNER_WIDTH 41

// Centre `s` in `width`, the way Python's str.center does.
static void print_centred(const char *colour, const char *s, int width) {
    int len = (int)strlen(s);
    int pad = (width - len) / 2;
    if (pad < 0) pad = 0;
    printf("%s%*s%s%s\n", colour, pad, "", s, C_RESET);
}

void banner_print(void) {
    printf("\n");
    for (unsigned i = 0; i < sizeof(kLogo) / sizeof(kLogo[0]); i++)
        printf("%s%s%s\n", kGradient[i % 7], kLogo[i], C_RESET);

    char ver[64];
    snprintf(ver, sizeof(ver), "RPCortex %s - %s", RPC_OS_VERSION, RPC_OS_CODENAME);
    print_centred(C_GRAY, ver, BANNER_WIDTH);

    // The rule: 41 box-drawing horizontals, as v1 drew it.
    printf("%s", C_GRAY);
    for (int i = 0; i < BANNER_WIDTH; i++) printf("─");
    printf("%s\n", C_RESET);

    printf("Initializing OS...\n\n");
}
