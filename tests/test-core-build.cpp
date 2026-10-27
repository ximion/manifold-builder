/*
 * Copyright (C) 2025-2026 Matthias Klumpp <matthias@tenstral.net>
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "core-build.hpp"
#include "sandbox.hpp"

#include <catch2/catch_test_macros.hpp>

using namespace mfb;

TEST_CASE("every core gets the Manifold configuration", "[core]")
{
    Script s;
    postprocess_core(s, "/tree", "x86_64-linux-gnu");
    const auto &text = s.text();
    CHECK(text.find("/tree/etc/ld.so.conf.d/manifold-app.conf") != std::string::npos);
    CHECK(text.find("/app/lib/x86_64-linux-gnu") != std::string::npos);
    CHECK(text.find("/tree/etc/fonts/conf.d/50-manifold-host-fonts.conf") != std::string::npos);
    CHECK(
        text.find("<remap-dir as-path=\"/usr/share/fonts\">/run/host/usr/share/fonts</remap-dir>")
        != std::string::npos);
}
