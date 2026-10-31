/*
 * Copyright (C) 2025-2026 Matthias Klumpp <matthias@tenstral.net>
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "app-build.hpp"
#include "bundle.hpp"
#include "core-build.hpp"
#include "error.hpp"
#include "logging.hpp"
#include "recipe.hpp"
#include "utils.hpp"

#include <glib.h>
#include <manifold.h>
#include <print>

int main(int argc, char **argv)
{
    gchar *output = nullptr;
    gchar *work_dir = nullptr;
    gchar *runtimes_dir = nullptr;
    gchar *sign_key = nullptr;
    gboolean no_sign = FALSE;
    gboolean keep_work = FALSE;
    gboolean no_cache = FALSE;
    gint jobs = 0;
    gboolean verbose = FALSE;
    gboolean show_version = FALSE;
    gchar **positional = nullptr;

    // clang-format off
    const GOptionEntry entries[] = {
        {"output", 'o', 0, G_OPTION_ARG_FILENAME,
         &output,
         "Output location for the built bundle", "FILE"
        },

        {"work-dir", 0, 0,
         G_OPTION_ARG_FILENAME,
         &work_dir,
         "Scratch directory [_build-<recipe name> beside the recipe]",
         "DIR"
        },

        {"runtimes-dir", 0, 0,
         G_OPTION_ARG_FILENAME,
         &runtimes_dir,
         "Where the core and platform a build sits on are: a Manifold store, or a directory of extracted bundles "
         "[/var/lib/manifold]",
         "DIR"
        },

        {"keep-work", 0, 0, G_OPTION_ARG_NONE,
         &keep_work,
         "Keep the work directory after the build", nullptr
        },

        {"no-cache", 0, 0, G_OPTION_ARG_NONE,
         &no_cache,
         "Fetch every source and package again, instead of keeping them for later builds", nullptr
        },

        {"jobs", 'j', 0, G_OPTION_ARG_INT,
         &jobs,
         "Number of parallel jobs for building parts [number of CPUs]", "N"
        },

        {"sign", 0, 0,
         G_OPTION_ARG_FILENAME,
         &sign_key,
         "Sign the bundle with this key: its name, the path of a key file, or the fingerprint of a key in ssh-agent "
         "[$MANIFOLD_SIGNING_KEY, or else the key named default]",
         "KEY"
        },

        {"no-sign", 0, 0, G_OPTION_ARG_NONE,
         &no_sign,
         "Do not sign the bundle, even if there is a key", nullptr
        },

        {"verbose", 'v', 0, G_OPTION_ARG_NONE,
         &verbose,
         "Show debug messages, among them every command that is run", nullptr
        },

        {"version", 0, 0, G_OPTION_ARG_NONE,
         &show_version,
         "Show the program version", nullptr
        },

        {G_OPTION_REMAINING, 0, 0, G_OPTION_ARG_FILENAME_ARRAY, &positional, nullptr, nullptr},

        {nullptr},
    };
    // clang-format on

    GOptionContext *context = g_option_context_new("build RECIPE | check BUNDLE");
    g_option_context_set_summary(
        context,
        "Builds the images a recipe describes and bundles them in a .manifold file,\n"
        "or checks a bundle the way Manifold does when installing it.");
    g_option_context_add_main_entries(context, entries, nullptr);

    GError *error = nullptr;
    if (!g_option_context_parse(context, &argc, &argv, &error)) {
        std::println(stderr, "manifold-builder: {}", error->message);
        g_error_free(error);
        return 2;
    }

    if (jobs < 0) {
        std::println(stderr, "manifold-builder: --jobs must not be negative");
        return 2;
    }

    if (sign_key && no_sign) {
        std::println(stderr, "manifold-builder: --sign and --no-sign can not be used together");
        return 2;
    }

    if (show_version) {
        std::println("manifold-builder {}", MFB_VERSION);
        return 0;
    }
    auto command = positional ? std::string_view(positional[0]) : std::string_view();
    auto arguments = positional ? g_strv_length(positional) - 1 : 0;
    if (!((command == "build" && arguments == 1) || (command == "check" && arguments == 1))) {
        gchar *help = g_option_context_get_help(context, TRUE, nullptr);
        std::print(stderr, "{}", help);
        g_free(help);
        return 2;
    }

    mfb::BuildOptions options;
    if (output)
        options.output = output;
    if (work_dir)
        options.work_dir = work_dir;
    if (runtimes_dir)
        options.runtimes_dir = runtimes_dir;
    options.keep_work = keep_work;
    if (!no_cache)
        options.cache_dir = mfb::fs::path(g_get_user_cache_dir()) / "manifold-builder";
    options.jobs = static_cast<unsigned>(jobs);
    g_option_context_free(context);

    mfb::log::init(verbose);
    quill::Logger *logRoot = mfb::log::root();
    try {
        if (command == "check") {
            mfb::verify_bundle(positional[1]);
            mfb::log::flush();
            return 0;
        }

        mfb::fs::path recipe_path = positional[1];
        auto recipe = mfb::load_recipe(recipe_path);

        g_autoptr(MfSigner) signer = no_sign ? nullptr : mfb::select_signer(sign_key);
        options.signer = signer;

        mfb::fs::path bundle;
        if (recipe.is_core())
            bundle = mfb::build_core(recipe, recipe_path, options);
        else
            bundle = mfb::build_app(recipe, recipe_path, options);

        LOG_INFO(logRoot, "Done: {}", bundle.string());
        mfb::log::flush();
        return 0;
    } catch (const std::exception &e) {
        LOG_ERROR(logRoot, "{}", e.what());
        mfb::log::flush();
        return 1;
    }
}
