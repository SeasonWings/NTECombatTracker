#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

namespace nte::combat_stats {

class AttributeColorMap {
public:
    [[nodiscard]] static AttributeColorMap Load(
        const std::filesystem::path& path,
        std::string& error);

    [[nodiscard]] std::optional<std::uint32_t> Find(
        std::string_view attribute) const;

private:
    std::unordered_map<std::string, std::uint32_t> colors_;
};

}  // namespace nte::combat_stats
