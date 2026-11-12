#include "selection/Identifier.hpp"

#include "core/Logging.hpp"

#include <algorithm>
#include <cctype>
#include <sstream>

namespace bes::identifier {
namespace {

/// The bundled table. Every row is a claim, so this stays deliberately small.
///
/// Rows are evidence-backed, not exhaustive: the point is to give the player a
/// correct starting set and to give the 1:Many index something real to expand.
/// Anything absent is still selectable, just reported as `Unverified` until the
/// game's own registry answers for it.
///
/// The class names on the entity rows are the RTTI names the mod actually reads
/// off live actors (`Rtti.cpp`), i.e. the leaf identifiers, not namespaced
/// spellings. Vanilla Bedrock ships several classes per spawnable type, which is
/// exactly why this is 1:Many — but only classes with a reason to be here are
/// listed, and unverified ones are omitted rather than guessed.
const std::vector<TableRow>& tableStorage() {
    static const std::vector<TableRow> rows = {
        // ---- blocks -------------------------------------------------------
        // Block identity comes from the game's own name table
        // (`Block::getTypeName` / the identifier stored on BlockType), so these
        // are safe to list: the runtime returns the same string shape.
        {Domain::Block, "minecraft:diamond_ore", {}, "vanilla identifier"},
        {Domain::Block, "minecraft:deepslate_diamond_ore", {}, "vanilla identifier"},
        {Domain::Block, "minecraft:emerald_ore", {}, "vanilla identifier"},
        {Domain::Block, "minecraft:deepslate_emerald_ore", {}, "vanilla identifier"},
        {Domain::Block, "minecraft:gold_ore", {}, "vanilla identifier"},
        {Domain::Block, "minecraft:deepslate_gold_ore", {}, "vanilla identifier"},
        {Domain::Block, "minecraft:iron_ore", {}, "vanilla identifier"},
        {Domain::Block, "minecraft:deepslate_iron_ore", {}, "vanilla identifier"},
        {Domain::Block, "minecraft:copper_ore", {}, "vanilla identifier"},
        {Domain::Block, "minecraft:deepslate_copper_ore", {}, "vanilla identifier"},
        {Domain::Block, "minecraft:coal_ore", {}, "vanilla identifier"},
        {Domain::Block, "minecraft:deepslate_coal_ore", {}, "vanilla identifier"},
        {Domain::Block, "minecraft:lapis_ore", {}, "vanilla identifier"},
        {Domain::Block, "minecraft:deepslate_lapis_ore", {}, "vanilla identifier"},
        {Domain::Block, "minecraft:redstone_ore", {}, "vanilla identifier"},
        {Domain::Block, "minecraft:deepslate_redstone_ore", {}, "vanilla identifier"},
        {Domain::Block, "minecraft:ancient_debris", {}, "vanilla identifier"},
        {Domain::Block, "minecraft:quartz_ore", {}, "vanilla identifier"},
        {Domain::Block, "minecraft:chest", {}, "vanilla identifier"},
        {Domain::Block, "minecraft:trapped_chest", {}, "vanilla identifier"},
        {Domain::Block, "minecraft:barrel", {}, "vanilla identifier"},
        {Domain::Block, "minecraft:furnace", {}, "vanilla identifier"},
        {Domain::Block, "minecraft:spawner", {}, "vanilla identifier"},
        {Domain::Block, "minecraft:water", {}, "vanilla identifier"},
        {Domain::Block, "minecraft:lava", {}, "vanilla identifier"},

        // ---- entities -----------------------------------------------------
        // Only the pairs whose class name is unambiguous. 1:Many rows are marked
        // as such; note that these are *documented* mappings (evidence: the
        // cross-build class/monster naming convention), not runtime-verified, so
        // they resolve to DocumentedTable, never RegistryConfirmed.
        {Domain::Entity, "minecraft:zombie", {"Zombie"}, "documented class mapping"},
        {Domain::Entity, "minecraft:husk", {"Husk"}, "documented class mapping"},
        {Domain::Entity, "minecraft:drowned", {"Drowned"}, "documented class mapping"},
        {Domain::Entity, "minecraft:skeleton", {"Skeleton"}, "documented class mapping"},
        {Domain::Entity, "minecraft:stray", {"Stray"}, "documented class mapping"},
        {Domain::Entity, "minecraft:wither_skeleton", {"WitherSkeleton"}, "documented class mapping"},
        {Domain::Entity, "minecraft:creeper", {"Creeper"}, "documented class mapping"},
        {Domain::Entity, "minecraft:spider", {"Spider"}, "documented class mapping"},
        {Domain::Entity, "minecraft:cave_spider", {"CaveSpider"}, "documented class mapping"},
        {Domain::Entity, "minecraft:enderman", {"Enderman"}, "documented class mapping"},
        {Domain::Entity, "minecraft:cow", {"Cow"}, "documented class mapping"},
        {Domain::Entity, "minecraft:pig", {"Pig"}, "documented class mapping"},
        {Domain::Entity, "minecraft:sheep", {"Sheep"}, "documented class mapping"},
        {Domain::Entity, "minecraft:chicken", {"Chicken"}, "documented class mapping"},
        {Domain::Entity, "minecraft:wolf", {"Wolf"}, "documented class mapping"},
        {Domain::Entity, "minecraft:cat", {"Cat"}, "documented class mapping"},
        {Domain::Entity, "minecraft:horse", {"Horse"}, "documented class mapping"},
        {Domain::Entity, "minecraft:villager", {"Villager", "VillagerV2"}, "documented class mapping (1:Many)"},
        {Domain::Entity, "minecraft:zombie_villager", {"ZombieVillager", "ZombieVillagerV2"},
         "documented class mapping (1:Many)"},
        {Domain::Entity, "minecraft:iron_golem", {"IronGolem"}, "documented class mapping"},
        {Domain::Entity, "minecraft:snow_golem", {"SnowGolem"}, "documented class mapping"},
        {Domain::Entity, "minecraft:blaze", {"Blaze"}, "documented class mapping"},
        {Domain::Entity, "minecraft:ghast", {"Ghast"}, "documented class mapping"},
        {Domain::Entity, "minecraft:slime", {"Slime"}, "documented class mapping"},
        {Domain::Entity, "minecraft:magma_cube", {"MagmaCube"}, "documented class mapping"},
        {Domain::Entity, "minecraft:witch", {"Witch"}, "documented class mapping"},
        {Domain::Entity, "minecraft:item", {"ItemActor"}, "documented class mapping"},
        {Domain::Entity, "minecraft:xp_orb", {"ExperienceOrb"}, "documented class mapping"},
        {Domain::Entity, "minecraft:arrow", {"Arrow"}, "documented class mapping"},
        {Domain::Entity, "minecraft:boat", {"Boat"}, "documented class mapping"},
        {Domain::Entity, "minecraft:minecart", {"Minecart"}, "documented class mapping"},
        // Players are matched by base class: every player-ish actor derives from
        // Player, and the mod's own RTTI classification already treats any class
        // containing "Player" as a player.
        {Domain::Entity, "minecraft:player", {"Player"}, "documented class mapping"},
    };
    return rows;
}

std::string key(Domain domain, const std::string& id) {
    return std::string(domainName(domain)) + "://" + id;
}

bool isLower(char c) {
    return std::islower(static_cast<unsigned char>(c)) != 0;
}

bool isPathChar(char c) {
    const auto value = static_cast<unsigned char>(c);
    return std::islower(value) || std::isdigit(value) || c == '_' || c == '.' || c == '-' || c == '/';
}

} // namespace

const char* domainName(Domain domain) {
    return domain == Domain::Block ? "block" : "entity";
}

const char* confidenceName(Confidence confidence) {
    switch (confidence) {
        case Confidence::Invalid: return "invalid";
        case Confidence::Unverified: return "unverified";
        case Confidence::ObservedLive: return "observed-live";
        case Confidence::DocumentedTable: return "documented";
        case Confidence::RegistryConfirmed: return "registry-confirmed";
        case Confidence::RegistryResolved: return "registry-resolved";
    }
    return "unknown";
}

const std::vector<TableRow>& documentedTable() {
    return tableStorage();
}

std::string toLower(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (const char c : text) out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    return out;
}

bool validSyntax(std::string_view canonical, Domain domain) {
    (void)domain; // Both domains use the same namespace:path grammar.

    if (canonical.size() < 3 || canonical.size() > 128) return false;

    const auto colon = canonical.find(':');
    if (colon == std::string_view::npos) return false;
    // Exactly one namespace separator; paths use '/', not ':'.
    if (canonical.find(':', colon + 1) != std::string_view::npos) return false;

    const auto nameSpace = canonical.substr(0, colon);
    const auto path = canonical.substr(colon + 1);
    if (nameSpace.empty() || path.empty()) return false;
    if (!isLower(nameSpace.front())) return false;

    for (const char c : nameSpace) {
        if (!(isLower(c) || std::isdigit(static_cast<unsigned char>(c)) || c == '_')) return false;
    }
    if (!isLower(path.front())) return false;
    for (const char c : path) {
        if (!isPathChar(c)) return false;
    }
    // No runs of separators, no trailing separator.
    if (path.back() == '.' || path.back() == '-' || path.back() == '/') return false;
    if (path.find("..") != std::string::npos || path.find("//") != std::string::npos) return false;

    return true;
}

std::string normalise(std::string_view raw, Domain domain) {
    // Trim first: pasting from a wiki or chat routinely carries whitespace.
    std::size_t begin = 0;
    std::size_t end = raw.size();
    while (begin < end && std::isspace(static_cast<unsigned char>(raw[begin]))) ++begin;
    while (end > begin && std::isspace(static_cast<unsigned char>(raw[end - 1]))) --end;
    if (begin >= end) return {};

    std::string text = toLower(raw.substr(begin, end - begin));

    // Tolerate the "<domain>/<id>" form the module menu itself uses.
    const std::string prefix = std::string(domainName(domain)) + "/";
    if (text.rfind(prefix, 0) == 0) text.erase(0, prefix.size());

    // Leading "minecraft:minecraft:x" style duplication is a real paste hazard.
    while (text.rfind("minecraft:minecraft:", 0) == 0) text.erase(0, 10);

    // A bare name gets the vanilla namespace, which is what players mean.
    if (text.find(':') == std::string::npos) text = "minecraft:" + text;

    // A trailing "minecraft:" with nothing after it is not salvageable.
    if (!validSyntax(text, domain)) return {};
    return text;
}

ClassIndex& ClassIndex::get() {
    static ClassIndex instance;
    return instance;
}

void ClassIndex::bind(Domain domain, const std::string& id, const std::string& className, Confidence confidence,
                      std::string evidence) {
    if (id.empty() || className.empty()) return;

    std::lock_guard lock(mMutex);
    auto& node = mNodes[key(domain, id)];
    auto [it, inserted] = node.classes.emplace(className, Binding{confidence, evidence});
    if (inserted) {
        core::log::debug("identifier: " + std::string(domainName(domain)) + " " + id + " -> class " + className +
                         " (" + confidenceName(confidence) + ")");
    } else if (confidence > it->second.confidence) {
        // Stronger evidence replaces weaker, including its explanation.
        it->second.confidence = confidence;
        if (!evidence.empty()) it->second.evidence = std::move(evidence);
    }
    if (confidence > node.confidence) {
        node.confidence = confidence;
        if (!evidence.empty()) node.evidence = std::move(evidence);
    }
}

void ClassIndex::note(Domain domain, const std::string& id, Confidence confidence, std::string evidence) {
    if (id.empty()) return;
    std::lock_guard lock(mMutex);
    auto& node = mNodes[key(domain, id)];
    if (confidence >= node.confidence) {
        node.confidence = confidence;
        if (!evidence.empty()) node.evidence = std::move(evidence);
    }
}

std::vector<std::string> ClassIndex::classesFor(Domain domain, const std::string& id) const {
    std::lock_guard lock(mMutex);
    const auto it = mNodes.find(key(domain, id));
    if (it == mNodes.end()) return {};

    std::vector<std::string> out;
    out.reserve(it->second.classes.size());
    for (const auto& [className, binding] : it->second.classes) out.push_back(className);
    // Deterministic order: the diagnostics dump and the tests both compare lists.
    std::sort(out.begin(), out.end());
    return out;
}

Confidence ClassIndex::confidenceFor(Domain domain, const std::string& id) const {
    std::lock_guard lock(mMutex);
    const auto it = mNodes.find(key(domain, id));
    return it == mNodes.end() ? Confidence::Unverified : it->second.confidence;
}

std::string ClassIndex::evidenceFor(Domain domain, const std::string& id) const {
    std::lock_guard lock(mMutex);
    const auto it = mNodes.find(key(domain, id));
    return it == mNodes.end() ? std::string{} : it->second.evidence;
}

bool ClassIndex::mapped(Domain domain, const std::string& id) const {
    std::lock_guard lock(mMutex);
    const auto it = mNodes.find(key(domain, id));
    return it != mNodes.end() && !it->second.classes.empty();
}

void ClassIndex::loadDocumentedTable() {
    {
        std::lock_guard lock(mMutex);
        if (mTableLoaded) return;
        mTableLoaded = true;
    }
    for (const auto& row : tableStorage()) {
        if (row.classes.empty()) {
            note(row.domain, row.id, Confidence::DocumentedTable, row.evidence);
            continue;
        }
        for (const char* className : row.classes) {
            bind(row.domain, row.id, className, Confidence::DocumentedTable, row.evidence);
        }
    }
    core::log::info("identifier: documented table loaded (" + std::to_string(tableStorage().size()) + " rows)");
}

std::string ClassIndex::describe() const {
    std::lock_guard lock(mMutex);
    if (mNodes.empty()) return "identifier index: empty\n";

    std::vector<std::string> keys;
    keys.reserve(mNodes.size());
    for (const auto& [name, node] : mNodes) keys.push_back(name);
    std::sort(keys.begin(), keys.end());

    std::ostringstream out;
    out << "identifier index: " << mNodes.size() << " entries\n";
    for (const auto& name : keys) {
        const auto& node = mNodes.at(name);
        out << "  " << name << " [" << confidenceName(node.confidence) << "]";
        if (node.classes.empty()) {
            out << " (no class mapping)";
        } else {
            std::vector<std::string> classes;
            for (const auto& [className, binding] : node.classes) {
                classes.push_back(className + ":" + confidenceName(binding.confidence));
            }
            std::sort(classes.begin(), classes.end());
            out << " ->";
            for (const auto& entry : classes) out << ' ' << entry;
        }
        if (!node.evidence.empty()) out << "  // " << node.evidence;
        out << '\n';
    }
    return out.str();
}

std::size_t ClassIndex::size() const {
    std::lock_guard lock(mMutex);
    return mNodes.size();
}

void ClassIndex::clear() {
    std::lock_guard lock(mMutex);
    mNodes.clear();
    mTableLoaded = false;
}

Entry resolve(Domain domain, std::string_view raw) {
    Entry entry;
    entry.domain = domain;
    entry.id = normalise(raw, domain);
    if (entry.id.empty()) {
        entry.confidence = Confidence::Invalid;
        entry.evidence = "not a valid identifier (expected namespace:path, e.g. minecraft:diamond_ore)";
        return entry;
    }

    auto& index = ClassIndex::get();
    index.loadDocumentedTable();
    entry.classes = index.classesFor(domain, entry.id);

    // Report what the index actually knows. A documented block has no class
    // mapping at all, which is normal for blocks and must not be mistaken for
    // "unknown".
    entry.confidence = index.confidenceFor(domain, entry.id);
    if (entry.confidence == Confidence::Unverified) {
        entry.evidence = "well-formed but not yet seen in this world";
    } else {
        entry.evidence = index.evidenceFor(domain, entry.id);
        if (!entry.classes.empty()) {
            entry.evidence += " (" + std::to_string(entry.classes.size()) + " class(es))";
        }
    }
    return entry;
}

} // namespace bes::identifier
