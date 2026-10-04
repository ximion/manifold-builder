/*
 * Copyright (C) 2025-2026 Matthias Klumpp <matthias@tenstral.net>
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "appstream.hpp"

#include "error.hpp"
#include "logging.hpp"
#include "utils.hpp"

#include <appstream-compose.h>
#include <appstream.h>
#include <format>
#include <manifold.h>

namespace mfb
{

quill::Logger *const logAppstream = log::logger("appstream");

namespace
{

/**
 * Convert plain text to AppStream description markup. Each paragraph becomes
 * a <p>, with its lines joined by spaces and special characters escaped.
 */
std::string description_markup(const std::string &text)
{
    std::string markup;
    std::string paragraph;
    auto end_paragraph = [&] {
        if (!paragraph.empty()) {
            g_autofree gchar *escaped = g_markup_escape_text(paragraph.c_str(), -1);
            markup += "<p>" + std::string(escaped) + "</p>";
        }
        paragraph.clear();
    };

    for_each_line(text, [&](std::string_view line) {
        line = trim(line);
        if (line.empty()) {
            end_paragraph();
            return;
        }
        if (!paragraph.empty())
            paragraph += ' ';
        paragraph += line;
    });
    end_paragraph();
    return markup;
}

/**
 * Write a component as MetaInfo XML, with all its translations.
 */
std::string metainfo_xml(AsComponent *cpt)
{
    g_autoptr(AsMetadata) metadata = as_metadata_new();
    as_metadata_set_locale(metadata, "ALL");
    as_metadata_add_component(metadata, cpt);

    g_autoptr(GError) error = nullptr;
    g_autofree gchar *xml = as_metadata_component_to_metainfo(metadata, AS_FORMAT_KIND_XML, &error);
    if (xml == nullptr)
        fail("Cannot write MetaInfo of {}: {}", as_component_get_id(cpt), error ? error->message : "unknown error");
    return xml;
}

/**
 * Convert a string from AppStream to std::string, NULL to an empty one.
 */
std::string to_string(const gchar *value)
{
    return value != nullptr ? value : "";
}

} // namespace

std::string generate_metainfo(const MetaInfo &info)
{
    g_autoptr(AsComponent) cpt = as_component_new();
    as_component_set_id(cpt, info.id.c_str());
    as_component_set_kind(cpt, info.kind == "addon" ? AS_COMPONENT_KIND_ADDON : AS_COMPONENT_KIND_RUNTIME);
    as_component_set_name(cpt, info.name.c_str(), "C");
    as_component_set_summary(cpt, info.summary.c_str(), "C");
    if (!info.description.empty())
        as_component_set_description(cpt, description_markup(info.description).c_str(), "C");
    as_component_set_metadata_license(cpt, info.metadata_license.c_str());
    if (!info.project_license.empty())
        as_component_set_project_license(cpt, info.project_license.c_str());
    if (!info.extends.empty())
        as_component_add_extends(cpt, info.extends.c_str());
    if (!info.homepage.empty())
        as_component_add_url(cpt, AS_URL_KIND_HOMEPAGE, info.homepage.c_str());

    g_autoptr(AsRelease) release = as_release_new();
    as_release_set_version(release, info.version.c_str());
    as_release_set_date(release, info.release_date.c_str());
    as_component_add_release(cpt, release);

    return metainfo_xml(cpt);
}

MetaInfo parse_metainfo(const std::string &xml)
{
    g_autoptr(AsMetadata) metadata = as_metadata_new();
    as_metadata_set_locale(metadata, "C");
    g_autoptr(GError) error = nullptr;
    if (!as_metadata_parse_data(metadata, xml.c_str(), -1, AS_FORMAT_KIND_XML, &error))
        fail("Cannot read MetaInfo: {}", error->message);
    AsComponent *cpt = as_metadata_get_component(metadata);
    if (cpt == nullptr)
        fail("The MetaInfo data describes no component");

    GPtrArray *extends = as_component_get_extends(cpt);
    return MetaInfo{
        .id = to_string(as_component_get_id(cpt)),
        .kind = to_string(as_component_kind_to_string(as_component_get_kind(cpt))),
        .extends = extends != nullptr && extends->len > 0
                       ? to_string(static_cast<const gchar *>(g_ptr_array_index(extends, 0)))
                       : "",
        .name = to_string(as_component_get_name(cpt)),
        .summary = to_string(as_component_get_summary(cpt)),
        .project_license = to_string(as_component_get_project_license(cpt)),
        .metadata_license = to_string(as_component_get_metadata_license(cpt)),
        .homepage = to_string(as_component_get_url(cpt, AS_URL_KIND_HOMEPAGE)),
    };
}

AppStreamCatalog::AppStreamCatalog(fs::path dir)
    : m_dir(std::move(dir)),
      m_metadata(as_metadata_new())
{
    fs::create_directories(m_dir);
    as_metadata_set_locale(m_metadata, "ALL");
    as_metadata_set_format_style(m_metadata, AS_FORMAT_STYLE_CATALOG);
    as_metadata_set_origin(m_metadata, MF_BUNDLE_CATALOG_ORIGIN);
}

AppStreamCatalog::~AppStreamCatalog()
{
    g_object_unref(m_metadata);
}

void AppStreamCatalog::add_tree(
    const fs::path &tree,
    const std::string &prefix,
    const std::string &component_id,
    const std::map<std::string, std::string> &custom)
{
    // we cache every icon size, a repository can still serve them remotely
    g_autoptr(AscIconPolicy) icon_policy = asc_icon_policy_new();
    AscIconPolicyIter iter;
    guint size = 0;
    guint scale = 0;
    AscIconState state = ASC_ICON_STATE_IGNORED;
    asc_icon_policy_iter_init(&iter, icon_policy);
    while (asc_icon_policy_iter_next(&iter, &size, &scale, &state)) {
        if (state != ASC_ICON_STATE_IGNORED)
            asc_icon_policy_set_policy(icon_policy, size, scale, ASC_ICON_STATE_CACHED_ONLY);
    }

    g_autoptr(AscCompose) compose = asc_compose_new();
    asc_compose_set_prefix(compose, prefix.c_str());
    asc_compose_add_allowed_cid(compose, component_id.c_str());
    asc_compose_set_origin(compose, MF_BUNDLE_CATALOG_ORIGIN);
    asc_compose_set_icon_policy(compose, icon_policy);

    // Manifold only accepts JPEG XL icons, so we do not rely on the default
    asc_compose_set_image_format(compose, ASC_IMAGE_FORMAT_JXL);
    asc_compose_set_icons_result_dir(compose, (m_dir / "icons").c_str());
    asc_compose_set_media_result_dir(compose, (m_dir / "media").c_str());
    // NOTE: Compose refuses to store screenshots without a media base URL. With
    // relative URLs it never writes the URL out, and a repository sets its own.
    asc_compose_set_media_baseurl(compose, "https://manifold.invalid/media");

    asc_compose_set_flags(
        compose,
        static_cast<AscComposeFlags>(
            ASC_COMPOSE_FLAG_VALIDATE | ASC_COMPOSE_FLAG_PROPAGATE_CUSTOM | ASC_COMPOSE_FLAG_ALLOW_NET
            | ASC_COMPOSE_FLAG_STORE_SCREENSHOTS | ASC_COMPOSE_FLAG_ALLOW_VIDEOS));
    g_autoptr(AscDirectoryUnit) unit = asc_directory_unit_new(tree.c_str());
    asc_compose_add_unit(compose, ASC_UNIT(unit));

    g_autoptr(GError) error = nullptr;
    GPtrArray *results = asc_compose_run(compose, nullptr, &error);
    if (results == nullptr)
        fail("appstream-compose failed on {}: {}", tree.string(), error ? error->message : "unknown error");

    for (guint i = 0; i < results->len; ++i) {
        auto *result = ASC_RESULT(g_ptr_array_index(results, i));
        GPtrArray *hints = asc_result_get_hints(result, component_id.c_str()); // owned by the result
        for (guint h = 0; hints != nullptr && h < hints->len; ++h) {
            auto *hint = ASC_HINT(g_ptr_array_index(hints, h));
            if (asc_hint_get_severity(hint) < AS_ISSUE_SEVERITY_WARNING)
                continue;
            g_autofree gchar *explanation = asc_hint_format_explanation_plain(hint);
            LOG_WARNING(logAppstream, "{}: {}: {}", component_id, asc_hint_get_tag(hint), explanation);
        }

        AsComponent *cpt = asc_result_get_component(result, component_id.c_str());
        if (cpt == nullptr)
            continue;
        for (const auto &[key, value] : custom)
            as_component_insert_custom_value(cpt, key.c_str(), value.c_str());
        as_metadata_add_component(m_metadata, cpt);
        return;
    }
    fail("The image has no usable AppStream metadata for {} (expected below {}/share/metainfo)", component_id, prefix);
}

fs::path AppStreamCatalog::write() const
{
    g_autoptr(GError) error = nullptr;
    g_autofree gchar *xml = as_metadata_components_to_catalog(m_metadata, AS_FORMAT_KIND_XML, &error);
    if (xml == nullptr)
        fail("Cannot write the AppStream catalog: {}", error ? error->message : "unknown error");
    auto file = m_dir / (std::string(MF_BUNDLE_CATALOG_ORIGIN) + ".xml");
    write_file(file, xml);
    return file;
}

std::map<std::string, std::string> manifold_custom(
    const std::string &architecture,
    const std::string &component,
    const std::string &version)
{
    return {{"manifold.id", std::format("{}/{}/{}", architecture, component, version)}};
}

std::vector<std::string> validate_metainfo(const std::string &xml)
{
    g_autoptr(AsValidator) validator = as_validator_new();
    as_validator_set_allow_net(validator, FALSE);
    if (as_validator_validate_data(validator, xml.c_str()))
        return {};

    std::vector<std::string> problems;
    g_autoptr(GList) issues = as_validator_get_issues(validator);
    for (GList *node = issues; node != nullptr; node = node->next) {
        auto *issue = AS_VALIDATOR_ISSUE(node->data);
        auto severity = as_validator_issue_get_severity(issue);
        if (severity != AS_ISSUE_SEVERITY_ERROR && severity != AS_ISSUE_SEVERITY_WARNING)
            continue;
        const gchar *hint = as_validator_issue_get_hint(issue);
        problems.push_back(
            std::format(
                "{}: {}{}",
                as_issue_severity_to_string(severity),
                as_validator_issue_get_tag(issue),
                hint ? std::string(" (") + hint + ")" : ""));
    }
    return problems;
}

} // namespace mfb
