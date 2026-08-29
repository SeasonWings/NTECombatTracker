#include "resource_name_resolver.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <fstream>
#include <unordered_map>
#include <utility>

namespace nte::combat_stats {

namespace {

struct CharacterEntry {
    std::uint32_t id{};
    std::string codename;
    std::string display_name;
    std::string attribute;
};

enum class ParentAbilityPolicy { SkillDamage, Semantic, Hidden };

struct EffectEntry {
    std::string ability;
    std::string component_name;
    ParentAbilityPolicy parent_policy{ParentAbilityPolicy::SkillDamage};
};

struct DamageSkillEntry {
    std::string technical_name;
    std::string ability;
};

[[nodiscard]] nlohmann::json ReadJson(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) throw std::runtime_error("required name resource is unavailable: " + path.string());
    return nlohmann::json::parse(stream);
}

[[nodiscard]] std::string Lower(std::string_view value) {
    std::string result(value);
    std::ranges::transform(result, result.begin(), [](const unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });
    return result;
}

[[nodiscard]] std::string CompactTechnicalName(const std::string_view path) {
    const auto slash = path.find_last_of("/\\");
    const auto dot = path.find_last_of('.');
    const auto position = slash == std::string_view::npos
        ? dot
        : (dot == std::string_view::npos ? slash : (std::max)(slash, dot));
    return std::string(position == std::string_view::npos ? path : path.substr(position + 1));
}

[[nodiscard]] bool IsUnknown(const std::string_view value) {
    return value.empty() || Lower(value).starts_with("unknown");
}

[[nodiscard]] std::string StripClassSuffix(std::string value) {
    if (value.size() > 2 && Lower(std::string_view(value).substr(value.size() - 2)) == "_c") {
        value.resize(value.size() - 2);
    }
    return value;
}

[[nodiscard]] bool IsIdentifier(const std::string_view value) {
    if (value.empty()) return false;
    return std::ranges::all_of(value, [](const unsigned char character) {
        return std::isalnum(character) != 0 || character == '_';
    });
}

[[nodiscard]] std::string ExtractGameplayEffect(const std::string_view path) {
    const auto marker = path.rfind(".Default__");
    auto candidate = marker == std::string_view::npos
        ? CompactTechnicalName(path)
        : std::string(path.substr(marker + 10));
    if (Lower(candidate).starts_with("default__")) candidate.erase(0, 9);
    candidate = StripClassSuffix(std::move(candidate));
    return IsIdentifier(candidate) && Lower(candidate).starts_with("ge_")
        ? candidate
        : std::string{};
}

}  // namespace

struct ResourceNameResolver::Entries {
    std::unordered_map<std::uint32_t, CharacterEntry> characters_by_id;
    std::unordered_map<std::string, std::vector<std::uint32_t>> character_ids_by_codename;
    std::unordered_map<std::string, std::string> enemies;
    std::unordered_map<std::string, DamageSkillEntry> damage_skills;
    std::unordered_map<std::string, std::string> ability_names;
    std::unordered_map<std::string, EffectEntry> effect_semantics;
};

std::string_view DisplayNameOriginName(const DisplayNameOrigin origin) noexcept {
    switch (origin) {
    case DisplayNameOrigin::Resource: return "resource";
    case DisplayNameOrigin::Technical: return "technical";
    case DisplayNameOrigin::Unknown: return "unknown";
    }
    return "unknown";
}

ResourceNameResolver ResourceNameResolver::Load(
    const std::filesystem::path& res_root,
    std::string& error) {
    error.clear();
    ResourceNameResolver result;
    std::string stage{"manifest"};
    try {
        const auto manifest = ReadJson(res_root / "data/asset_manifest.json");
        result.version_ = manifest.at("assets_git_revision").get<std::string>();
        if (result.version_.empty()) throw std::runtime_error("asset manifest revision is empty");

        stage = "characters";
        const auto characters = ReadJson(res_root / "data/characters/characters.json");
        stage = "enemies";
        const auto enemies = ReadJson(res_root / "data/enemies/enemies.json");
        stage = "skill damage";
        const auto damage = ReadJson(res_root / "data/skills/skill_damage.json");
        stage = "ability tips";
        const auto abilities = ReadJson(res_root / "data/skills/ability_tips.json");
        stage = "effect semantics";
        const auto semantics = ReadJson(
            res_root / "data/skills/gameplay_effect_semantics.json");

        auto entries = std::make_shared<Entries>();
        stage = "character entries";
        for (const auto& [id_text, value] : characters.at("characters").items()) {
            std::size_t consumed{};
            const auto parsed = std::stoul(id_text, &consumed);
            if (consumed != id_text.size() || parsed > UINT32_MAX) {
                throw std::runtime_error("character id is invalid: " + id_text);
            }
            CharacterEntry entry{
                static_cast<std::uint32_t>(parsed),
                value.at("codename").get<std::string>(),
                value.at("name_zh").get<std::string>(),
                value.value("attribute", std::string{})};
            if (entry.codename.empty() || entry.display_name.empty()) {
                throw std::runtime_error(
                    "character codename or Chinese name is empty: " + id_text);
            }
            entries->character_ids_by_codename[Lower(entry.codename)].push_back(entry.id);
            if (!entries->characters_by_id.emplace(entry.id, std::move(entry)).second) {
                throw std::runtime_error("duplicate character id: " + id_text);
            }
        }
        stage = "enemy entries";
        for (const auto& value : enemies.at("enemies")) {
            const auto id = value.at("config_id").get<std::string>();
            const auto name = value.at("name_zh").get<std::string>();
            if (id.empty() || name.empty() ||
                !entries->enemies.emplace(Lower(id), name).second) {
                throw std::runtime_error("enemy config id or Chinese name is invalid: " + id);
            }
        }
        stage = "skill damage entries";
        if (!damage.is_array() || damage.empty() || !damage.front().is_object()) {
            throw std::runtime_error("skill damage resource root is invalid");
        }
        for (const auto& [effect, value] : damage.front().at("Rows").items()) {
            if (const auto ability = value.find("GAName"); ability != value.end() &&
                ability->is_string() && !ability->get_ref<const std::string&>().empty() &&
                ability->get_ref<const std::string&>() != "None") {
                entries->damage_skills.emplace(Lower(effect), DamageSkillEntry{
                    effect, ability->get<std::string>()});
            }
        }
        stage = "ability tip entries";
        for (const auto& [ability, value] : abilities.at("abilities").items()) {
            const auto name = value.value("name_zh", std::string{});
            if (!name.empty()) entries->ability_names.emplace(ability, name);
        }
        stage = "effect semantic entries";
        for (const auto& [effect, value] : semantics.at("effects").items()) {
            EffectEntry entry;
            entry.ability = value.value("ability", std::string{});
            entry.component_name = value.value("damage_name_zh", std::string{});
            if (const auto show = value.find("show_parent_ability"); show != value.end()) {
                entry.parent_policy = show->get<bool>()
                    ? ParentAbilityPolicy::Semantic
                    : ParentAbilityPolicy::Hidden;
            }
            entries->effect_semantics.emplace(Lower(effect), std::move(entry));
        }
        result.entries_ = std::move(entries);
    } catch (const std::exception& exception) {
        result.version_.clear();
        result.entries_.reset();
        error = stage + ": " + exception.what();
    }
    return result;
}

ResolvedParticipantName ResourceNameResolver::ResolveParticipant(
    const std::string_view technical_path) const {
    ResolvedParticipantName result;
    result.technical_name.assign(technical_path);
    result.resource_version = version_;
    if (IsUnknown(technical_path)) {
        result.display_name = "Unknown";
        return result;
    }

    auto compact = CompactTechnicalName(technical_path);
    const auto without_suffix = StripClassSuffix(compact);
    if (entries_) {
        const auto lower = Lower(without_suffix);
        if (lower.starts_with("player_")) {
            const auto first = without_suffix.find('_');
            const auto second = without_suffix.find('_', first + 1);
            if (second != std::string::npos) {
                const auto number = without_suffix.substr(first + 1, second - first - 1);
                const auto codename = without_suffix.substr(second + 1);
                std::uint32_t candidate{};
                try {
                    std::size_t consumed{};
                    const auto parsed = std::stoul(number, &consumed);
                    if (consumed == number.size() && parsed <= 999) {
                        candidate = static_cast<std::uint32_t>(1000 + parsed);
                    }
                } catch (const std::exception&) {
                }
                if (const auto found = entries_->characters_by_id.find(candidate);
                    found != entries_->characters_by_id.end() &&
                    Lower(found->second.codename) == Lower(codename)) {
                    result.display_name = found->second.display_name;
                    result.attribute = found->second.attribute;
                    result.character_id = found->second.id;
                    result.origin = DisplayNameOrigin::Resource;
                    return result;
                }
                if (const auto found = entries_->character_ids_by_codename.find(Lower(codename));
                    found != entries_->character_ids_by_codename.end() && found->second.size() == 1) {
                    const auto& character = entries_->characters_by_id.at(found->second.front());
                    result.display_name = character.display_name;
                    result.attribute = character.attribute;
                    result.character_id = character.id;
                    result.origin = DisplayNameOrigin::Resource;
                    return result;
                }
            }
        }
        if (const auto enemy = entries_->enemies.find(lower); enemy != entries_->enemies.end()) {
            result.display_name = enemy->second;
            result.origin = DisplayNameOrigin::Resource;
            return result;
        }
    }
    result.display_name = std::move(compact);
    result.origin = DisplayNameOrigin::Technical;
    return result;
}

ResolvedDamageName ResourceNameResolver::ResolveDamageSource(
    const std::string_view technical_path) const {
    ResolvedDamageName result;
    result.technical_name.assign(technical_path);
    result.resource_version = version_;
    if (IsUnknown(technical_path)) {
        result.display_name = "Unknown";
        return result;
    }
    result.gameplay_effect_name = ExtractGameplayEffect(technical_path);
    if (!entries_ || result.gameplay_effect_name.empty()) {
        result.display_name = result.gameplay_effect_name.empty()
            ? CompactTechnicalName(technical_path)
            : result.gameplay_effect_name;
        result.origin = DisplayNameOrigin::Technical;
        return result;
    }

    const auto effect_key = Lower(result.gameplay_effect_name);
    if (const auto found = entries_->damage_skills.find(effect_key);
        found != entries_->damage_skills.end()) {
        result.gameplay_effect_name = found->second.technical_name;
        result.ability_name = found->second.ability;
    }
    std::string component;
    bool show_parent = true;
    if (const auto found = entries_->effect_semantics.find(effect_key);
        found != entries_->effect_semantics.end()) {
        component = found->second.component_name;
        show_parent = found->second.parent_policy != ParentAbilityPolicy::Hidden;
        if ((found->second.parent_policy == ParentAbilityPolicy::Semantic ||
             result.ability_name.empty()) &&
            !found->second.ability.empty()) {
            result.ability_name = found->second.ability;
        }
    }
    std::string parent;
    if (const auto found = entries_->ability_names.find(result.ability_name);
        found != entries_->ability_names.end()) {
        parent = found->second;
    }
    if (!show_parent && !component.empty()) {
        result.display_name = component;
    } else if (!parent.empty() && !component.empty()) {
        result.display_name = parent + " · " + component;
    } else if (!parent.empty()) {
        result.display_name = parent;
    } else if (!component.empty()) {
        result.display_name = component;
    } else if (!result.ability_name.empty()) {
        result.display_name = result.ability_name;
    } else {
        result.display_name = result.gameplay_effect_name;
    }
    result.origin = result.display_name == result.gameplay_effect_name ||
            result.display_name == result.ability_name
        ? DisplayNameOrigin::Technical
        : DisplayNameOrigin::Resource;
    return result;
}

}  // namespace nte::combat_stats
