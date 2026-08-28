#pragma once

#include <filesystem>
#include <memory>
#include <string>
#include <string_view>

namespace nte::combat_stats {

enum class NameOrigin { Catalog, Technical, Unknown };

struct ResolvedDisplayName {
    std::string technical_name;
    std::string display_name;
    NameOrigin origin{NameOrigin::Unknown};
    std::string catalog_version;
};

[[nodiscard]] std::string_view NameOriginName(NameOrigin origin) noexcept;

class NameCatalog {
public:
    struct Entries;
    [[nodiscard]] static NameCatalog Load(
        const std::filesystem::path& path,
        std::string_view game_build,
        std::string& error);

    [[nodiscard]] ResolvedDisplayName Character(std::string_view path) const;
    [[nodiscard]] ResolvedDisplayName DamageSource(std::string_view path) const;
    [[nodiscard]] const std::string& version() const noexcept { return version_; }

private:
    std::string version_;
    std::shared_ptr<const Entries> entries_;
};

}  // namespace nte::combat_stats
