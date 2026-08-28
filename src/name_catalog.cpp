#include "name_catalog.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <fstream>
#include <unordered_map>

namespace nte::combat_stats {

struct NameCatalog::Entries {
    std::unordered_map<std::string, std::string> characters;
    std::unordered_map<std::string, std::string> damage_sources;
};

namespace {

std::string CompactTechnicalName(const std::string_view path) {
    const auto slash = path.find_last_of("/\\");
    const auto dot = path.find_last_of('.');
    const auto position = slash == std::string_view::npos
        ? dot
        : (dot == std::string_view::npos ? slash : (std::max)(slash, dot));
    return std::string(position == std::string_view::npos ? path : path.substr(position + 1));
}

void MergeEntries(
    const nlohmann::json& group,
    std::unordered_map<std::string, std::string>& characters,
    std::unordered_map<std::string, std::string>& sources) {
    const auto merge = [](const nlohmann::json& values, auto& destination) {
        for (const auto& [key, value] : values.items()) {
            if (key.empty() || key.front() != '/' || !value.is_string() ||
                value.get_ref<const std::string&>().empty()) {
                throw std::runtime_error("name catalog entries require object paths and names");
            }
            destination.insert_or_assign(key, value.get<std::string>());
        }
    };
    if (const auto values = group.find("characters"); values != group.end()) {
        merge(*values, characters);
    }
    if (const auto values = group.find("damageSources"); values != group.end()) {
        merge(*values, sources);
    }
}

ResolvedDisplayName Resolve(
    const std::shared_ptr<const NameCatalog::Entries>& entries,
    const std::unordered_map<std::string, std::string> NameCatalog::Entries::* member,
    const std::string& version,
    const std::string_view technical_name) {
    ResolvedDisplayName result;
    result.technical_name.assign(technical_name);
    result.catalog_version = version;
    if (technical_name.empty() || technical_name.starts_with("Unknown")) {
        result.display_name = "Unknown";
        result.origin = NameOrigin::Unknown;
        return result;
    }
    if (entries) {
        const auto& values = entries.get()->*member;
        if (const auto found = values.find(std::string(technical_name)); found != values.end()) {
            result.display_name = found->second;
            result.origin = NameOrigin::Catalog;
            return result;
        }
    }
    result.display_name = CompactTechnicalName(technical_name);
    result.origin = NameOrigin::Technical;
    return result;
}

}  // namespace

std::string_view NameOriginName(const NameOrigin origin) noexcept {
    switch (origin) {
    case NameOrigin::Catalog: return "catalog";
    case NameOrigin::Technical: return "technical";
    case NameOrigin::Unknown: return "unknown";
    }
    return "unknown";
}

NameCatalog NameCatalog::Load(
    const std::filesystem::path& path,
    const std::string_view game_build,
    std::string& error) {
    error.clear();
    NameCatalog result;
    try {
        std::ifstream stream(path, std::ios::binary);
        if (!stream) throw std::runtime_error("name catalog is unavailable");
        const auto document = nlohmann::json::parse(stream);
        if (document.at("schemaVersion").get<std::uint32_t>() != 1 ||
            document.at("locale").get<std::string>() != "zh-CN") {
            throw std::runtime_error("name catalog schema or locale is unsupported");
        }
        result.version_ = document.at("catalogVersion").get<std::string>();
        if (result.version_.empty()) throw std::runtime_error("name catalog version is empty");
        auto entries = std::make_shared<Entries>();
        const auto& builds = document.at("builds");
        if (const auto defaults = builds.find("default"); defaults != builds.end()) {
            MergeEntries(*defaults, entries->characters, entries->damage_sources);
        }
        if (!game_build.empty()) {
            if (const auto exact = builds.find(std::string(game_build)); exact != builds.end()) {
                MergeEntries(*exact, entries->characters, entries->damage_sources);
            }
        }
        result.entries_ = std::move(entries);
    } catch (const std::exception& exception) {
        error = exception.what();
    }
    return result;
}

ResolvedDisplayName NameCatalog::Character(const std::string_view path) const {
    return Resolve(entries_, &Entries::characters, version_, path);
}

ResolvedDisplayName NameCatalog::DamageSource(const std::string_view path) const {
    return Resolve(entries_, &Entries::damage_sources, version_, path);
}

}  // namespace nte::combat_stats
