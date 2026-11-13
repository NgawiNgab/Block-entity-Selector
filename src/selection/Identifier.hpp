#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace bes::identifier {

/// Block identifier ("minecraft:diamond_ore") or entity identifier
/// ("minecraft:zombie"). Deliberately a plain value type: the registry layer
/// never touches a game pointer, so it can be tested on the host and cannot
/// destabilise the process.
enum class Domain : std::uint8_t {
    Block,
    Entity,
};

const char* domainName(Domain domain);

/// Where a statement about an identifier came from. Ordered from weakest to
/// strongest so callers can compare with `>=`.
///
/// The distinction is the whole point of this layer: a typed identifier is a
/// *claim*, and the UI must never present a claim as a fact.
enum class Confidence : std::uint8_t {
    /// Not a syntactically valid identifier. Rejected before it can be selected.
    Invalid = 0,
    /// Well-formed, but nothing has verified it. The mod will match it against
    /// the world and report what it saw; it will not claim the id exists.
    Unverified = 1,
    /// The identifier was observed on a live object in this session: a block was
    /// actually read with this id, or an actor class mapped to it was present.
    ObservedLive = 2,
    /// Listed in the bundled table, which is compiled from evidence rather than
    /// guessed. Still not a runtime registry check.
    DocumentedTable = 3,
    /// Answered by the game's own registry lookup. The only level that proves
    /// the identifier exists in this build.
    RegistryConfirmed = 4,
    /// As above, and the registry supplied the backing object/class set.
    RegistryResolved = 5,
};

const char* confidenceName(Confidence confidence);

/// Result of validating one identifier. `classes` is the 1:Many expansion for
/// entities: one identifier may be backed by several RTTI classes.
struct Entry {
    Domain domain = Domain::Block;
    std::string id;
    Confidence confidence = Confidence::Unverified;
    /// RTTI class names that this entity identifier is known to cover. Evidence
    /// class names are added to this at runtime; the table only carries what was
    /// established by analysis. Empty for blocks.
    std::vector<std::string> classes;
    /// Why we believe what we believe; printed verbatim by the diagnostics dump
    /// so nothing has to be taken on trust.
    std::string evidence;

    explicit operator bool() const noexcept { return !id.empty() && confidence != Confidence::Invalid; }
};

/// One row of the bundled, evidence-backed table.
struct TableRow {
    Domain domain;
    const char* id;
    /// Optional; entity rows may legitimately have no class mapping yet.
    std::vector<const char*> classes;
    const char* evidence;
};

/// Every row the project has actual evidence for. Kept short on purpose: an
/// entry here is a claim, and inventing rows would defeat the confidence system.
const std::vector<TableRow>& documentedTable();

/// Normalises user input into a canonical identifier.
///
/// Accepts what people actually paste: surrounding whitespace, a leading
/// "block/" or "entity/" selector, mixed case, and the bare name without the
/// namespace (which gets "minecraft:"). Returns an empty string when the input
/// cannot be a valid identifier, so a bad paste can never become a selection.
std::string normalise(std::string_view raw, Domain domain);

/// Checks syntax only (namespace:path, lower-case, legal characters). This is
/// what `normalise` calls; exposed separately for the tests and the UI.
bool validSyntax(std::string_view canonical, Domain domain);

/// Lower-case, trimmed copy used for display and comparison. Kept separate so a
/// pretty name can preserve the user's own capitalisation if we ever want to.
std::string toLower(std::string_view text);

/// The 1:Many entity identifier -> RTTI class index.
///
/// Kept apart from `Selection`/`RTTI` on purpose: the scanner asks this index
/// which classes to match, and this index asks whoever actually knows (the
/// documented table now, a registry lookup later) what backs an identifier.
/// Adding a new evidence source means adding a `bind` call, not editing the
/// scanner.
class ClassIndex {
public:
    static ClassIndex& get();

    /// Registers a class for an identifier. Duplicate pairs are ignored; the
    /// first binding's evidence is kept because it is usually the strongest
    /// (table or registry before runtime observation).
    void bind(Domain domain, const std::string& id, const std::string& className, Confidence confidence,
              std::string evidence = {});

    /// Classes currently known for an identifier, or empty when unmapped.
    std::vector<std::string> classesFor(Domain domain, const std::string& id) const;

    /// True when at least one class is bound, i.e. entity matching can work.
    bool mapped(Domain domain, const std::string& id) const;

    /// Best confidence recorded for an identifier. This is not derived from the
    /// class count: a documented block has no classes but is still documented.
    Confidence confidenceFor(Domain domain, const std::string& id) const;

    /// Evidence text recorded for an identifier, or empty.
    std::string evidenceFor(Domain domain, const std::string& id) const;

    /// Records an evidence trail entry without inventing a class, used when an
    /// identifier is observed live but no class can be resolved for it.
    void note(Domain domain, const std::string& id, Confidence confidence, std::string evidence);

    /// Seeding the bundled table. Safe to call repeatedly.
    void loadDocumentedTable();

    /// Diagnostics: one line per known identifier.
    std::string describe() const;

    std::size_t size() const;

    void clear();

private:
    ClassIndex() = default;
    struct Binding {
        Confidence confidence = Confidence::Unverified;
        std::string evidence;
    };
    struct Node {
        /// class name -> how we know about it
        std::unordered_map<std::string, Binding> classes;
        Confidence confidence = Confidence::Unverified;
        std::string evidence;
    };

    mutable std::mutex mMutex;
    std::unordered_map<std::string, Node> mNodes;
    bool mTableLoaded = false;
};

/// Convenience: resolve and, when the id is syntactically valid, upgrade the
/// index with whatever evidence is available right now.
Entry resolve(Domain domain, std::string_view raw);

} // namespace bes::identifier
