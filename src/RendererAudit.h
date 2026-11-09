#pragma once
#include <array>
#include <cstdint>
#include <string_view>

namespace bsc {
// Entries correspond exactly to native BlockActorRendererId 0..27.
// "cpu_model" only means a native CPU model extractor exists. It DOES NOT
// mean that every animation, skin, texture atlas, render pass or UV is covered.
struct RendererAuditEntry {
    std::string_view name;
    std::string_view capturePath;
};
inline constexpr std::array<RendererAuditEntry,28> RendererAudit{{
    {"Default","runtime_only"},
    {"Chest","direct_model_hook"},
    {"Sign","direct_model_hook"},
    {"MobSpawner","runtime_only"},
    {"Skull","cpu_model"},
    {"Enchanter","cpu_model"},
    {"PistonArm","direct_model_hook"},
    {"ItemFrame","runtime_only"},
    {"MovingBlock","runtime_only"},
    {"Chalkboard","cpu_model"},
    {"Beacon","runtime_only"},
    {"EndGateway","runtime_only"},
    {"EnderChest","direct_model_hook"},
    {"ShulkerBox","cpu_model"},
    {"CommandBlock","runtime_only"},
    {"Bed","cpu_model"},
    {"Banner","cpu_model"},
    {"Conduit","runtime_only"},
    {"Lectern","cpu_model"},
    {"Bell","cpu_model"},
    {"Campfire","runtime_only"},
    {"HangingSign","direct_model_hook"},
    {"DecoratedPot","cpu_model"},
    {"BrushableBlock","runtime_only"},
    {"TrialSpawner","runtime_only"},
    {"Vault","runtime_only"},
    {"Shelf","runtime_only"},
    {"CopperGolemStatue","cpu_model"}
}};
}
