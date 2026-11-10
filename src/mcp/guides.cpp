// SPDX-License-Identifier: GPL-3.0-or-later
#include "guides.h"
#include <QRegularExpression>

namespace nova::mcp::guides {
namespace {
constexpr auto Verified = "1.21.11";

// Every 1.21.11 statement below was checked against the official 1.21.11 client resources
// (client SHA-1 ba2df812c2d12e0219c489c4cd9a5e1f0760f5bd); see minecraft-1.21.11-facts.json.
// Statements about Nova describe Nova's behaviour, not the game's.
const char *header =
    "Applies to Minecraft Java Edition 1.21.11 only (resource pack format 75.0). Checked against the official "
    "1.21.11 client resources. Do not apply these rules to other versions without checking them.\n\n";

struct Entry {
    const char *topic, *title, *body;
};
const Entry entries[] = {
    {"pack", "Resource paths, namespaces and pack metadata",
     "## Resource paths, not filesystem paths\n"
     "- Nova identifies assets by resource-pack paths relative to the pack root, such as "
     "`assets/minecraft/textures/item/diamond_sword.png`. Never pass drive letters, `..`, backslashes or absolute paths.\n"
     "- A resource identifier is `namespace:path`, for example `minecraft:item/diamond_sword`. "
     "The namespace is the folder under `assets/`.\n"
     "- Nova accepts lowercase `a-z`, `0-9`, `_`, `-` and `.` in namespaces, plus `/` in paths.\n\n"
     "## Where each kind lives (1.21.11)\n"
     "| Kind | Path | Example |\n|---|---|---|\n"
     "| Texture | `assets/<ns>/textures/<path>.png` | `minecraft:item/diamond_sword` → `assets/minecraft/textures/item/diamond_sword.png` |\n"
     "| Model | `assets/<ns>/models/<path>.json` | `minecraft:item/diamond_sword` → `assets/minecraft/models/item/diamond_sword.json` |\n"
     "| Item definition | `assets/<ns>/items/<item>.json` | `assets/minecraft/items/diamond_sword.json` |\n"
     "| Blockstate | `assets/<ns>/blockstates/<block>.json` | `assets/minecraft/blockstates/stone.json` |\n"
     "| Equipment (worn armor) | `assets/<ns>/equipment/<id>.json` | `assets/minecraft/equipment/diamond.json` |\n"
     "| Atlas | `assets/<ns>/atlases/<id>.json` | `assets/minecraft/atlases/armor_trims.json` |\n"
     "| Animation metadata | `<texture>.png.mcmeta` beside the PNG | `assets/minecraft/textures/block/magma.png.mcmeta` |\n\n"
     "- To replace a vanilla resource, use the same namespace and path. Use your own namespace for new resources.\n\n"
     "## pack.mcmeta\n"
     "- The 1.21.11 resource pack format is **75.0**.\n"
     "- Nova's Project Properties writes `min_format` and `max_format` as `[75, 0]` for a 1.21.11 target. Nova's "
     "validator rejects a `pack_format`-only declaration of format 65 or later, and `supported_formats` in such ranges.\n"
     "- Read the pack's target with `nova.project.info` (`minecraftVersion`, `packFormat`, `minFormat`, `maxFormat`). "
     "If it is missing or ambiguous, ask the user which version the pack targets.\n"},
    {"items", "Item definitions, models and textures",
     "## The chain: item definition → model → texture\n"
     "- Each item has an item definition at `assets/<ns>/items/<item>.json`. The 1.21.11 client has 1,505 of them. "
     "The definition chooses the model.\n"
     "- Simple case, `items/diamond_sword.json`: model type `minecraft:model` with model `minecraft:item/diamond_sword`.\n"
     "- Conditional cases use other types:\n"
     "  - `items/diamond_chestplate.json` is `minecraft:select` on `minecraft:trim_material`, with fallback "
     "`minecraft:item/diamond_chestplate`.\n"
     "  - `items/elytra.json` is `minecraft:condition` on `minecraft:broken`.\n"
     "- Model `models/item/diamond_sword.json` has parent `minecraft:item/handheld` and `textures.layer0` "
     "`minecraft:item/diamond_sword`.\n"
     "- Flat items use parent `minecraft:item/generated`, for example `models/item/diamond_chestplate.json`. "
     "`item/handheld` has parent `item/generated`, which has parent `builtin/generated`.\n\n"
     "## Choosing what to edit\n"
     "- To restyle an item, edit the texture its model references (`layer0`, and `layer1` for overlays).\n"
     "- To change its shape, held pose or variants, inspect the item definition and the model chain first.\n"
     "- Use `nova.assets.context` (relationships `parent`, `model`, `texture`) and `nova.model.inspect`. "
     "Do not assume the visible texture is the only asset involved.\n"},
    {"textures", "Texture sizes, models and companion assets",
     "## Sizes in the 1.21.11 client\n"
     "- `block/stone.png`, `item/diamond_sword.png` and `item/diamond_chestplate.png`: 16×16.\n"
     "- Worn armor `entity/equipment/humanoid/diamond.png` and `entity/equipment/humanoid_leggings/diamond.png`: 64×32.\n"
     "- Keep a replacement texture's size and layout unless you also change the models or metadata that use it. "
     "Model faces select regions of the texture, so moving pixels can break the mapping.\n\n"
     "## Companion assets to check\n"
     "- Animation metadata: `<texture>.png.mcmeta` (see the `animations` guide).\n"
     "- Models that reference the texture, item definitions that select those models, and blockstates for blocks. "
     "For example `blockstates/stone.json` uses the models `minecraft:block/stone` and `minecraft:block/stone_mirrored`.\n"
     "- Equipment definitions and trim atlases for armor (see `armor` and `trims`).\n\n"
     "## In Nova\n"
     "- `nova.assets.context` lists direct and reverse references.\n"
     "- `nova.asset.preview_image` with view `uv` shows a model face's mapping.\n"
     "- `nova.pack.validate` checks references and metadata, including open unsaved documents.\n"},
    {"armor", "Armor: item icons, worn textures and equipment",
     "Armor has two separate looks. Changing one does not change the other.\n\n"
     "## Inventory icon\n"
     "- Item definition `assets/minecraft/items/diamond_chestplate.json`, fallback model `minecraft:item/diamond_chestplate`.\n"
     "- That model has parent `minecraft:item/generated` and `layer0` `minecraft:item/diamond_chestplate`, "
     "which is `assets/minecraft/textures/item/diamond_chestplate.png` (16×16).\n\n"
     "## Worn armor\n"
     "- Equipment definition `assets/minecraft/equipment/diamond.json` has the layers `horse_body`, `humanoid`, "
     "`humanoid_leggings` and `nautilus_body`. Each refers to the texture `minecraft:diamond`.\n"
     "- The worn textures are `assets/minecraft/textures/entity/equipment/humanoid/diamond.png` and "
     "`assets/minecraft/textures/entity/equipment/humanoid_leggings/diamond.png`, both 64×32.\n"
     "- `humanoid` and `humanoid_leggings` are separate textures. Edit both to restyle a full set.\n"
     "- `equipment/elytra.json` uses a `wings` layer with `use_player_texture: true`.\n\n"
     "Trims are separate resources, see `trims`.\n"},
    {"trims", "Armor trims",
     "Trims are separate resources. Do not paint trim patterns into armor textures.\n\n"
     "## Worn trims\n"
     "- Patterns: `assets/minecraft/textures/trims/entity/humanoid/<pattern>.png` and "
     "`.../trims/entity/humanoid_leggings/<pattern>.png`, for example "
     "`textures/trims/entity/humanoid/coast.png`, 64×32.\n"
     "- Colours come from palettes. `assets/minecraft/atlases/armor_trims.json` uses a "
     "`minecraft:paletted_permutations` source with palette key `minecraft:trims/color_palettes/trim_palette` "
     "and 16 material permutations: amethyst, copper, copper_darker, diamond, diamond_darker, emerald, gold, "
     "gold_darker, iron, iron_darker, lapis, netherite, netherite_darker, quartz, redstone, resin.\n"
     "- Palettes are 8×1 PNGs in `assets/minecraft/textures/trims/color_palettes/`. The key palette is "
     "`textures/trims/color_palettes/trim_palette.png`.\n\n"
     "## Trims on item icons\n"
     "- The items atlas (`assets/minecraft/atlases/items.json`) applies the same palette key to "
     "`minecraft:trims/items/helmet_trim`, `chestplate_trim`, `leggings_trim` and `boots_trim` (16×16), for "
     "example `textures/trims/items/chestplate_trim.png`.\n"
     "- A trimmed icon is a second model layer. `models/item/diamond_chestplate_quartz_trim.json` has `layer0` "
     "`minecraft:item/diamond_chestplate` and `layer1` `minecraft:trims/items/chestplate_trim_quartz`.\n"
     "- `items/diamond_chestplate.json` selects these models by `minecraft:trim_material`.\n"
     "- Changing a palette changes every trim that uses that material.\n"},
    {"animations", "Animated textures: frames and metadata",
     "## Frame layout\n"
     "- An animated texture is a PNG plus `<texture>.png.mcmeta` with an `animation` object.\n"
     "- Frames are stacked vertically and are as wide as the texture. Without a size in the metadata, frames are square:\n"
     "  - `block/magma.png` is 16×48: 3 frames.\n"
     "  - `block/sea_lantern.png` is 16×80: 5 frames.\n"
     "- Non-square frames set `height` (or `width`). `entity/conduit/wind.png` is 64×704 with `\"height\": 32`, "
     "so it has 22 frames of 64×32.\n\n"
     "## Metadata fields\n"
     "- `frametime`: game ticks per frame. Examples: magma 8, sea_lantern 5, prismarine 300.\n"
     "- `interpolate`: blend between frames. Magma and prismarine use `true`.\n"
     "- `frames`: playback order. Entries are frame indexes or objects with `index` and `time`. Prismarine lists "
     "22 entries over its 4 frames (16×64).\n\n"
     "## In Nova\n"
     "- Nova edits frames and timing natively.\n"
     "- Saving an animation writes the PNG and its `.png.mcmeta` together (one column, vertical) and saves the "
     "editable animation. Check the save result for both.\n"},
    {"pvp", "PvP-oriented packs",
     "Nova guidance for packs used in player-versus-player play. These are policy recommendations, not game facts.\n\n"
     "- **Keep silhouettes recognizable.** Players, armor, weapons, projectiles and blocks should keep their "
     "shapes so others' gear and actions read correctly.\n"
     "- **Keep material tiers distinct.** Leather, chainmail, iron, gold, diamond and netherite should stay "
     "distinguishable in icons and in worn armor.\n"
     "- **No x-ray-style visibility.** Do not make blocks transparent or see-through, remove block faces, or add "
     "outlines or glows that reveal ores, players or entities behind blocks. Nova will not help create such packs.\n"
     "- **Server rules.** Packs that give this kind of visibility or advantage may violate multiplayer server "
     "rules and can lead to bans. Users should check each server's rules.\n"},
};
const Entry *find(const QString &topic) {
    for (const auto &entry : entries)
        if (topic == entry.topic)
            return &entry;
    return nullptr;
}
} // namespace

QStringList guideVersions() { return {Verified}; }
QStringList novaVersions() { return {"1.21.4", "1.21.9", "1.21.11", "26.1", "26.3"}; }
QStringList topics() {
    QStringList list;
    for (const auto &entry : entries)
        list << entry.topic;
    return list;
}
QString title(const QString &topic) {
    const auto entry = find(topic);
    return entry ? QString::fromUtf8(entry->title) : QString{};
}
QString uri(const QString &version, const QString &topic) { return "nova://rules/" + version + '/' + topic; }
Lookup guide(const QString &version, const QString &topic) {
    Lookup result;
    const auto entry = find(topic);
    if (!guideVersions().contains(version)) {
        result.code = novaVersions().contains(version) ? "guide_unavailable" : "unsupported_version";
        result.message = novaVersions().contains(version)
            ? QString("No verified Nova guide exists for Minecraft %1 yet. Guides are available for: %2. "
                      "Do not apply them to %1.").arg(version, guideVersions().join(", "))
            : QString("Minecraft %1 is not a version Nova supports. Nova supports %2; guides are available for %3.")
                  .arg(version.left(16), novaVersions().join(", "), guideVersions().join(", "));
        result.details = {{"pointer", "/version"}, {"guideVersions", QJsonArray::fromStringList(guideVersions())},
                          {"novaVersions", QJsonArray::fromStringList(novaVersions())}};
        return result;
    }
    if (!entry) {
        result.code = "unknown_topic";
        result.message = QString("No guide topic named \"%1\". Topics: %2.").arg(topic.left(32), topics().join(", "));
        result.details = {{"pointer", "/topic"}, {"topics", QJsonArray::fromStringList(topics())}};
        return result;
    }
    result.guide = {uri(version, topic), topic, version, QString::fromUtf8(entry->title),
                    "# " + QString::fromUtf8(entry->title) + " — Minecraft Java " + version + "\n\n" +
                        QString::fromUtf8(header) + QString::fromUtf8(entry->body)};
    return result;
}
bool parseUri(const QString &text, QString *version, QString *topic) {
    static const QRegularExpression form("^nova://rules/([0-9]{1,3}(?:\\.[0-9]{1,3}){1,2})/([a-z][a-z0-9_-]{0,31})$");
    const auto match = form.match(text);
    if (!match.hasMatch())
        return false;
    *version = match.captured(1);
    *topic = match.captured(2);
    return true;
}
QString indexText() {
    QString text = "# Nova resource-pack guides\n\n"
                   "Read `nova://rules/<version>/<topic>` or call `nova.guide.get` with the same topic and version.\n\n"
                   "Guides are available for: " + guideVersions().join(", ") + ".\n"
                   "Nova can target: " + novaVersions().join(", ") + ". Versions without guides have no "
                   "verified Nova guidance yet; check their rules before applying any.\n\n| Topic | Title |\n|---|---|\n";
    for (const auto &entry : entries)
        text += QString("| `%1` | %2 |\n").arg(entry.topic, QString::fromUtf8(entry.title));
    return text;
}
QJsonObject catalog() {
    QJsonArray list;
    for (const auto &entry : entries) {
        QJsonArray uris;
        for (const auto &version : guideVersions())
            uris.append(uri(version, entry.topic));
        list.append(QJsonObject{{"topic", entry.topic}, {"title", QString::fromUtf8(entry.title)},
                                {"versions", QJsonArray::fromStringList(guideVersions())}, {"uris", uris}});
    }
    return {{"guideVersions", QJsonArray::fromStringList(guideVersions())},
            {"novaVersions", QJsonArray::fromStringList(novaVersions())},
            {"topics", list}, {"uriTemplate", "nova://rules/{version}/{topic}"}, {"index", IndexUri}};
}
QJsonArray resources() {
    QJsonArray list{QJsonObject{{"uri", IndexUri}, {"name", "rules-index"}, {"title", "Nova guide index"},
                                {"description", "Topics and Minecraft versions with Nova guides."},
                                {"mimeType", "text/markdown"}}};
    for (const auto &version : guideVersions())
        for (const auto &entry : entries)
            list.append(QJsonObject{{"uri", uri(version, entry.topic)},
                {"name", QString("rules-%1-%2").arg(version, entry.topic)},
                {"title", QString("%1 (Minecraft %2)").arg(QString::fromUtf8(entry.title), version)},
                {"description", QString("Nova guide for Minecraft Java %1. Read-only.").arg(version)},
                {"mimeType", "text/markdown"}});
    list.append(QJsonObject{{"uri", StyleNotesUri}, {"name", "project-style-notes"},
        {"title", "Pack style notes"},
        {"description", "The user's optional style brief for the authorized pack (user content, read-only)."},
        {"mimeType", "application/json"}});
    return list;
}
QJsonArray resourceTemplates() {
    return {QJsonObject{{"uriTemplate", "nova://rules/{version}/{topic}"}, {"name", "rules"},
        {"title", "Nova guide by version and topic"},
        {"description", "Topics: " + topics().join(", ") + ". Versions: " + guideVersions().join(", ") + "."},
        {"mimeType", "text/markdown"}}};
}
QString instructions() {
    return "Nova Studio edits Minecraft Java resource packs.\n"
           "- Asset identifiers are resource-pack paths (assets/<namespace>/...) or namespace:path ids, "
           "never filesystem paths.\n"
           "- Check the pack's Minecraft version (nova.project.info) or ask the user before applying "
           "version-specific rules. Guides: nova.guide.list, nova.guide.get, or nova://rules/<version>/<topic>.\n"
           "- Edits and new assets change open documents in memory. A successful edit is not proof that it "
           "was saved to the pack.\n"
           "- Saving a texture writes it to its resource path in the pack. Use nova.drafts.save or "
           "nova.drafts.save_all, or ask the user to use Save or Save All in Nova, and check the saved and "
           "failed lists in the result.\n"
           "- Inspect item definitions, models, blockstates, companion textures and .png.mcmeta animation "
           "metadata (nova.asset.relationships, nova.assets.context); the visible texture is rarely the only asset "
           "involved. Treat unresolved or possible links as unknown, not as facts.\n"
           "- nova.vanilla.list and nova.vanilla.texture read Nova's local vanilla texture cache only: textures, no "
           "models, never a download.\n"
           "- Pack style notes (nova://project/style-notes) are the user's preferences, not permissions.\n"
           "- Access is limited to the project the user authorized. Respect read-only grants, revocation and "
           "project changes; never try to work around Nova's authorization or the user's consent.";
}
} // namespace nova::mcp::guides
