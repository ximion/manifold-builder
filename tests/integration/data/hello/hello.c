/*
 * Copyright (C) 2025-2026 Matthias Klumpp <matthias@tenstral.net>
 *
 * SPDX-License-Identifier: MIT
 */

/* One program, built with every build system the builder knows. The
 * strings are what the integration tests look for in the binaries. */

#include <stdio.h>

#define GREETING "greeting: unpatched"

int main(void) {
#ifdef MANIFOLD_CFLAGS_OK
    puts("manifold-cflags-ok");
#endif
    puts(GREETING);
    return 0;
}
