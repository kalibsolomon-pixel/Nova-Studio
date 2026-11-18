// SPDX-License-Identifier: GPL-3.0-or-later
#include "render_layers.h"
#include "models.h"
#include <QRegularExpression>

namespace nova::mc {
RenderLayer blockRenderLayer(const QString &blockId) {
    const auto id = Location::parse(blockId);
    if (id.nameSpace != "minecraft")
        return RenderLayer::Solid;
    const auto &name = id.path;
    if (name == "tripwire")
        return RenderLayer::Tripwire;
    static const QSet<QString> translucent{
        "glass", "glass_pane", "tinted_glass", "ice", "frosted_ice", "nether_portal",
        "slime_block", "honey_block", "redstone_wire", "bubble_column", "water"};
    if (translucent.contains(name) || name.endsWith("_stained_glass") ||
        name.endsWith("_stained_glass_pane"))
        return RenderLayer::Translucent;

    // 1.21.11 uses one terrain cutout layer, with mipmapping supplied by the atlas sampler.
    static const QSet<QString> cutout{
        "activator_rail", "amethyst_cluster", "attached_melon_stem", "attached_pumpkin_stem",
        "azalea", "bamboo", "beacon", "beetroots", "big_dripleaf", "big_dripleaf_stem",
        "brewing_stand", "bush", "cactus", "cactus_flower", "calibrated_sculk_sensor",
        "campfire", "carrots", "cave_vines", "cave_vines_plant", "chorus_flower", "chorus_plant",
        "cobweb", "cocoa", "comparator", "conduit", "copper_lantern", "cornflower",
        "crimson_fungus", "crimson_roots", "dandelion", "dead_bush", "detector_rail",
        "end_rod", "fern", "fire", "firefly_bush", "flower_pot", "flowering_azalea",
        "frogspawn", "glow_lichen", "grass_block", "hanging_roots", "hopper", "iron_bars",
        "iron_chain", "kelp", "kelp_plant", "ladder", "lantern", "large_amethyst_bud",
        "large_fern", "leaf_litter", "lilac", "lily_of_the_valley", "lily_pad",
        "mangrove_propagule", "mangrove_roots", "medium_amethyst_bud", "melon_stem",
        "nether_sprouts", "nether_wart", "pale_hanging_moss", "pale_moss_carpet", "peony",
        "pink_petals", "pitcher_crop", "pitcher_plant", "pointed_dripstone", "poppy", "potatoes",
        "powered_rail", "pumpkin_stem", "rail", "repeater", "resin_clump", "rose_bush",
        "scaffolding", "sculk_sensor", "sculk_shrieker", "sculk_vein", "sea_pickle", "seagrass",
        "short_dry_grass", "short_grass", "small_amethyst_bud", "small_dripleaf", "soul_campfire",
        "soul_fire", "soul_lantern", "spawner", "spore_blossom", "stonecutter", "sugar_cane",
        "sunflower", "sweet_berry_bush", "tall_dry_grass", "tall_grass", "tall_seagrass",
        "torchflower", "torchflower_crop", "trial_spawner", "tripwire_hook", "turtle_egg",
        "twisting_vines", "twisting_vines_plant", "vault", "vine", "warped_fungus",
        "warped_roots", "weeping_vines", "weeping_vines_plant", "wheat", "wildflowers",
        "allium", "azure_bluet", "blue_orchid", "brown_mushroom", "closed_eyeblossom",
        "open_eyeblossom", "orange_tulip", "oxeye_daisy", "pink_tulip", "red_mushroom",
        "red_tulip", "white_tulip", "wither_rose", "torch"};
    if (cutout.contains(name))
        return RenderLayer::Cutout;
    static const QRegularExpression families(
        "^(?:(?:oak|spruce|birch|jungle|acacia|dark_oak|cherry|pale_oak|mangrove|bamboo|"
        "crimson|warped|iron)_(?:trapdoor|door)|(?:waxed_)?(?:exposed_|weathered_|oxidized_)?"
        "copper_(?:bars|chain|grate|door|trapdoor)|(?:dead_)?(?:tube|brain|bubble|fire|horn)"
        "_coral(?:_fan|_wall_fan)?|(?:redstone_|soul_|copper_)?(?:wall_)?torch)$");
    if (families.match(name).hasMatch() || name.startsWith("potted_") ||
        name.endsWith("_leaves") || name.endsWith("_sapling") || name.endsWith("_bed"))
        return RenderLayer::Cutout;
    return RenderLayer::Solid;
}

RenderLayer itemRenderLayer(const QString &blockId, bool itemAtlas) {
    return itemAtlas || blockId.isEmpty() || blockRenderLayer(blockId) == RenderLayer::Translucent
               ? RenderLayer::Translucent : RenderLayer::Cutout;
}

float alphaCutoff(RenderLayer layer, bool terrain) {
    if (!terrain)
        return .1f;
    switch (layer) {
    case RenderLayer::Solid: return 0.f;
    case RenderLayer::Cutout: return .5f;
    case RenderLayer::Translucent: return .01f;
    case RenderLayer::Tripwire: return .1f;
    }
    return .1f;
}
} // namespace nova::mc
