#include "attribute_color_map.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>

namespace {

int failures{};

void Check(const bool condition, const char* message) {
    if (condition) return;
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
}

void RepositoryDictionaryProvidesAllAttributeColors() {
    std::string error;
    const auto colors = nte::combat_stats::AttributeColorMap::Load(
        std::filesystem::path(NTE_TEST_SOURCE_DIR) / "dict/attribute_colors.json",
        error);

    Check(error.empty(), "the checked-in attribute color dictionary should load");
    Check(colors.Find("灵") == 0x2F9F89U, "灵 should use its teal representative color");
    Check(colors.Find("相") == 0xB67817U, "相 should use its gold representative color");
    Check(colors.Find("暗") == 0x625DAFU, "暗 should use its violet representative color");
    Check(colors.Find("光") == 0x699A87U, "光 should use its mint representative color");
    Check(colors.Find("魂") == 0x326FBFU, "魂 should use its blue representative color");
    Check(colors.Find("咒") == 0xB61E3AU, "咒 should use its crimson representative color");
    Check(!colors.Find("未知").has_value(),
        "an unknown attribute should not invent a representative color");
}

std::filesystem::path TemporaryRoot() {
    const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto root = std::filesystem::temp_directory_path() /
        ("nte-attribute-colors-test-" + std::to_string(nonce));
    std::filesystem::create_directories(root);
    return root;
}

void Write(const std::filesystem::path& path, const std::string_view content) {
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    stream << content;
}

void LowercaseHexIsAccepted() {
    const auto root = TemporaryRoot();
    const auto path = root / "attribute_colors.json";
    Write(path, R"({"灵":"#2f9f89","相":"#b67817","暗":"#625daf","光":"#699a87","魂":"#326fbf","咒":"#b61e3a"})");
    std::string error;
    const auto colors = nte::combat_stats::AttributeColorMap::Load(path, error);
    Check(error.empty() && colors.Find("灵") == 0x2F9F89U &&
              colors.Find("咒") == 0xB61E3AU,
        "lowercase hexadecimal colors should be accepted");
    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
}

void InvalidDictionariesDegradeSafely() {
    const auto root = TemporaryRoot();
    const auto path = root / "attribute_colors.json";
    std::string error;

    auto colors = nte::combat_stats::AttributeColorMap::Load(path, error);
    Check(!error.empty() && !colors.Find("灵").has_value(),
        "a missing dictionary should return an empty map with an error");

    Write(path, R"({"灵":"#2F9F89","相":"#B67817","暗":"#625DAF","光":"#699A87","魂":"#326FBF"})");
    colors = nte::combat_stats::AttributeColorMap::Load(path, error);
    Check(!error.empty() && !colors.Find("灵").has_value(),
        "a dictionary missing a required attribute should be rejected atomically");

    Write(path, R"({"灵":"#2F9F89","相":"#B67817","暗":"#625DAF","光":"#699A87","魂":"#326FBF","咒":""})");
    colors = nte::combat_stats::AttributeColorMap::Load(path, error);
    Check(!error.empty() && !colors.Find("灵").has_value(),
        "an empty color should reject the complete dictionary");

    Write(path, R"({"灵":"#2F9F89","相":"#B67817","暗":"#625DAF","光":"#699A87","魂":"#326FBF","咒":"#B61E3Z"})");
    colors = nte::combat_stats::AttributeColorMap::Load(path, error);
    Check(!error.empty() && !colors.Find("灵").has_value(),
        "a non-hexadecimal color should reject the complete dictionary");

    Write(path, R"({"灵":"#2F9F89","灵":"#000000","相":"#B67817","暗":"#625DAF","光":"#699A87","魂":"#326FBF","咒":"#B61E3A"})");
    colors = nte::combat_stats::AttributeColorMap::Load(path, error);
    Check(!error.empty() && !colors.Find("灵").has_value(),
        "duplicate attribute keys should be rejected");

    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
}

}  // namespace

int main() {
    RepositoryDictionaryProvidesAllAttributeColors();
    LowercaseHexIsAccepted();
    InvalidDictionariesDegradeSafely();
    if (failures != 0) return 1;
    std::cout << "All attribute color map tests passed\n";
    return 0;
}
