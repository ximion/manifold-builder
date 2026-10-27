/*
 * Copyright (C) 2025-2026 Matthias Klumpp <matthias@tenstral.net>
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "core-build.hpp"
#include "sandbox.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

using namespace mfb;
using Catch::Matchers::ContainsSubstring;

TEST_CASE("every core gets the Manifold configuration", "[core]")
{
    Script s;
    postprocess_core(s, "/tree", "x86_64-linux-gnu");
    const auto &text = s.text();
    CHECK_THAT(text, ContainsSubstring("/tree/etc/ld.so.conf.d/manifold-app.conf"));
    CHECK_THAT(text, ContainsSubstring("/app/lib/x86_64-linux-gnu"));
    CHECK_THAT(text, ContainsSubstring("/tree/etc/fonts/conf.d/50-manifold-host-fonts.conf"));
    CHECK_THAT(
        text, ContainsSubstring("<remap-dir as-path=\"/usr/share/fonts\">/run/host/usr/share/fonts</remap-dir>"));
}
