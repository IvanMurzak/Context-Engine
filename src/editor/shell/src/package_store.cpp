// The package store — `<home>/.context/packages` enumeration, the manifest -> `Contribution` mapping,
// and the mount projection. See package_store.h for the three decisions, the manifest shape and the
// five validation rules; see ext_scheme.h § mount PROVENANCE for the containment check every root
// here has already passed.

#include "context/editor/shell/package_store.h"

#include "json_number_read.h" // the shared range-guarded numeric read (float-cast-overflow UB guard)

#include "context/editor/contract/json.h"
#include "context/editor/gui/contract/panel_state.h" // kStateSchemaVersionKey — the D6 state key
#include "context/editor/gui/contract/registry.h"    // manifest_defect — the registry's own verdict
#include "context/editor/shell/keybindings_bridge.h" // home_directory() — the ONE home resolver

#include <algorithm>
#include <cstdint>
#include <fstream>
#include <optional>
#include <sstream>
#include <system_error>

namespace context::editor::shell
{
namespace fs = std::filesystem;
namespace gc = gui::contract;
using contract::Json;

namespace
{

// Read a small file into `out`. False on any IO error OR when the file exceeds the cap — an oversized
// manifest is treated as unreadable rather than loaded (package_store.h: untrusted input with no
// bound is an allocation an attacker chooses). Mirrors the sibling readers in user_config.cpp /
// themes_bridge.cpp / keybindings_bridge.cpp — and it is the FOURTH copy of those ~20 lines, which is
// worth naming rather than justifying. The per-module cap is NOT the reason it cannot be shared: a
// caller-supplied bound is exactly the shape `json_number_read.h` uses one directory over, for the same
// three-copies-drifted problem, and each caller still names its own `kMax…Bytes` at the call site. The
// honest reason is scope — unifying the family touches three TUs outside this change. Recorded as
// follow-up work, not as a rule.
[[nodiscard]] bool read_small_file(const fs::path& path, std::uintmax_t size, std::string& out)
{
    if (size > kMaxPackageManifestBytes)
    {
        return false;
    }
    // C++ streams, never std::fopen: MSVC /W4 /WX rejects the C stdio family as C4996 and the local
    // GCC gate cannot see it (conventions.md § Coding conventions).
    std::ifstream stream(path, std::ios::binary);
    if (!stream)
    {
        return false;
    }
    std::ostringstream buffer;
    buffer << stream.rdbuf();
    if (stream.bad())
    {
        return false;
    }
    out = buffer.str();
    return true;
}

// --- the permissive readers (mirroring panels.ts's `readString` / `readNumber`) --------------------
// ⚠ There is no `read_bool` any more. It had exactly one caller — `dock.singleton` — and manifest v3
// removed that member, so it went with it rather than staying as a helper nothing calls (which under
// -Werror is not even a style question: an unused static function is a build failure).

[[nodiscard]] std::string read_string(const Json& source, const char* key,
                                      const std::string& fallback = std::string())
{
    if (!source.is_object() || !source.contains(key))
    {
        return fallback;
    }
    const Json& value = source.at(key);
    return value.is_string() ? value.as_string() : fallback;
}

// RANGE-GUARDED, through the Shell's ONE numeric reader (json_number_read.h) — never `as_int()`
// directly. `Json::as_int()` is an unguarded `static_cast<std::int64_t>` of the stored double, and a
// package manifest is the most untrusted input this file has: `Json::parse` accepts `1e300` happily,
// and casting that to an integral type is UNDEFINED BEHAVIOUR, which the blocking
// `sanitize (ASan+UBSan, ubuntu)` leg reports as `float-cast-overflow`. The check runs on the DOUBLE
// before any cast, so guarding cannot happen after the UB. Each caller passes the bounds of the type
// it casts to, which is what makes ITS cast defined; absent / non-number / NaN / out-of-range all read
// the same permissive way — "no usable number" — and take `fallback`, so every caller's own
// downstream refusal keeps deciding, and none of them is made unreachable by this guard.
[[nodiscard]] std::int64_t read_int(const Json& source, const char* key, std::int64_t lo,
                                   std::int64_t hi, std::int64_t fallback = 0)
{
    const std::optional<double> raw =
        detail::number_in_range(source, key, static_cast<double>(lo), static_cast<double>(hi));
    return raw ? static_cast<std::int64_t>(*raw) : fallback;
}

// A manifest string array, read STRICTLY: a non-string entry is a defect the caller refuses, never an
// entry silently dropped. That is the opposite direction from `readStringArray` in panels.ts, and
// deliberately so — that parser reads a projection the SHELL produced, where a non-string can only be
// a Shell defect; this one reads a third-party file, where dropping an entry would present the package
// to a consent surface as declaring less than it wrote down (the same reasoning rule (d) states for
// capabilities).
[[nodiscard]] bool read_string_array(const Json& source, const char* key,
                                     std::vector<std::string>& out, std::string& bad_entry)
{
    if (!source.is_object() || !source.contains(key))
    {
        return true; // absent = declared nothing
    }
    const Json& value = source.at(key);
    if (!value.is_array())
    {
        bad_entry = "<not an array>";
        return false;
    }
    for (std::size_t i = 0; i < value.size(); ++i)
    {
        const Json& entry = value.at(i);
        if (!entry.is_string())
        {
            bad_entry = "<non-string entry #" + std::to_string(i) + ">";
            return false;
        }
        out.push_back(entry.as_string());
    }
    return true;
}

[[nodiscard]] const Json& read_object(const Json& source, const char* key)
{
    static const Json empty = Json::object();
    if (!source.is_object() || !source.contains(key) || !source.at(key).is_object())
    {
        return empty;
    }
    return source.at(key);
}

// The manifest's `dock.zone` token -> DockZone.
// DERIVED FROM THE FORWARD TABLE, for the same reason `read_kind` below is: a hand-written second copy
// of a closed vocabulary is a copy that can drift, and one of the two copies in the first draft of this
// file already had. The tokens happen to agree today, so this is not a bug fix — it is removing the
// only way it could become one (a renamed token, or a new `DockZone` enumerator, silently mapping to
// `center`). `panels.ts` reaches its own inverse the same way, by searching the closed list.
[[nodiscard]] gc::DockZone read_dock_zone(const Json& dock)
{
    const std::string token = read_string(dock, "zone", "center");
    for (const gc::DockZone zone : {gc::DockZone::left, gc::DockZone::right, gc::DockZone::top,
                                    gc::DockZone::bottom, gc::DockZone::center})
    {
        if (token == gc::dock_zone_token(zone))
        {
            return zone;
        }
    }
    // An unrecognised zone falls back to `center`, exactly as `readDock` in panels.ts does: the
    // vocabulary is closed, so anything else is drift rather than a new zone, and the cost of the
    // fallback is cosmetic (where a panel first appears). Contrast `content.type`, which fails CLOSED
    // because the cost there is not cosmetic.
    return gc::DockZone::center;
}

// The manifest's `kind` token -> ContributionKind, falling back to `panel`. A package contributing an
// inspector or a gizmo is a designed part of the R-EDIT-001 contract (extension.h), so the vocabulary
// is read rather than pinned to panels — but an unrecognised token becomes `panel`, the kind with no
// `target` semantics, rather than being trusted into a target-keyed lookup.
// DERIVED FROM THE FORWARD TABLE, never restated. This function is only an INVERSE if it accepts
// exactly the tokens `gc::contribution_kind_token` emits, and a hand-written second table does not
// stay an inverse: the first draft of this one matched `asset_kind_editor` (the C++ enumerator's
// spelling) while the projection emits `asset-kind-editor` (extension.cpp), so a manifest written
// against the editor's OWN output silently read as `panel`. Comparing against the forward table makes
// that class of drift unrepresentable — and `panels.ts` reaches its inverse the same way, by searching
// the closed token list rather than repeating it.
[[nodiscard]] gc::ContributionKind read_kind(const Json& source)
{
    const std::string token = read_string(source, "kind", "panel");
    for (const gc::ContributionKind kind :
         {gc::ContributionKind::panel, gc::ContributionKind::inspector, gc::ContributionKind::gizmo,
          gc::ContributionKind::asset_kind_editor})
    {
        if (token == gc::contribution_kind_token(kind))
        {
            return kind;
        }
    }
    // An unrecognised token becomes `panel` — the kind with no `target` semantics — rather than being
    // trusted into a target-keyed lookup.
    return gc::ContributionKind::panel;
}

// Is `id` namespaced to `package_id` — either exactly the package id, or `<package-id>.<something>`?
// Rule (a) of package_store.h: without it a package could contribute `builtin.inspector`.
[[nodiscard]] bool id_is_namespaced_to(const std::string& id, const std::string& package_id)
{
    if (id == package_id)
    {
        return true;
    }
    const std::string prefix = package_id + ".";
    return id.size() > prefix.size() && id.compare(0, prefix.size(), prefix) == 0;
}

// Rule (c): the panel entry must be a `context-ext://<this-package>/…` URL.
//
// SPELLED OUT RATHER THAN REUSING `ExtAssetResolver::resolve`, because the two ask different
// questions: resolve() asks "does this URL name a servable asset in a MOUNTED package", which at
// manifest-read time is both unanswerable (nothing is mounted yet) and the wrong bar (the entry file
// may legitimately not exist until the panel opens). What matters here is the ORIGIN — that the entry
// names THIS package's host and no other's — which is a pure string property of the URL.
[[nodiscard]] bool entry_is_own_origin(const std::string& entry, const std::string& package_id)
{
    const std::string prefix = std::string(kExtUrlPrefix) + package_id;
    if (entry.size() <= prefix.size() || entry.compare(0, prefix.size(), prefix) != 0)
    {
        return false;
    }
    // The authority must END at the package id — otherwise `context-ext://pkg-evil/...` would pass as
    // `pkg`'s own origin by prefix. A '/' is the only byte that may follow.
    return entry[prefix.size()] == '/';
}

} // namespace

// ------------------------------------------------------------------------- the canonical store root

fs::path package_store_root()
{
    // The SAME home resolver `user_config_path()`, `keybindings_path()` and the themes directory use
    // (`USERPROFILE` on Windows, `HOME` on POSIX, nullopt when neither is set). One resolver, so the
    // four members of `~/.context/` cannot disagree about where the user's home is.
    const std::optional<fs::path> home = home_directory();
    if (!home.has_value())
    {
        return {};
    }
    return *home / ".context" / kPackageStoreDirName;
}

// ------------------------------------------------------------------------------ the manifest -> C++

bool read_package_manifest(const fs::path& manifest_file, const std::string& expected_package_id,
                           const fs::path& package_root, InstalledPackage& out,
                           std::string& error_code, std::string& message)
{
    out = InstalledPackage{};
    error_code.clear();
    message.clear();

    // ⚠ THE MANIFEST MUST NOT BE AN OS LINK — the SAME refusal the package root itself gets
    // (ext_scheme.h § mount PROVENANCE), applied to the one file this module actually OPENS. The mount
    // provenance walk stops AT the package root, so nothing above covers a link INSIDE it, and both
    // `is_regular_file` and `std::ifstream` follow links: a package shipping
    // `context-package.json -> /etc/passwd` would otherwise have up to `kMaxPackageManifestBytes` of an
    // arbitrary readable file read into this process, with derived content handed back to the operator
    // channel (the id-mismatch message echoes the target's `id` verbatim, and a parse failure reports a
    // byte offset — a weak content oracle). Refused by NAME, before anything follows it.
    std::error_code ec;
    if (path_is_os_link(manifest_file))
    {
        error_code = kErrManifestMissing;
        message = std::string(kPackageManifestFileName) +
                  " is an OS link; a package's manifest must be a real file inside its own root";
        return false;
    }
    if (!fs::is_regular_file(manifest_file, ec))
    {
        error_code = kErrManifestMissing;
        message = "no " + std::string(kPackageManifestFileName) + " at the package root";
        return false;
    }
    ec.clear();
    const std::uintmax_t size = fs::file_size(manifest_file, ec);
    std::string text;
    if (ec || !read_small_file(manifest_file, size, text))
    {
        error_code = kErrManifestMissing;
        message = std::string(kPackageManifestFileName) +
                  " could not be read, or exceeds the manifest size cap";
        return false;
    }

    Json document;
    try
    {
        document = Json::parse(text);
    }
    catch (const std::exception& error)
    {
        error_code = kErrManifestMalformed;
        message = std::string(kPackageManifestFileName) + " is not valid JSON: " + error.what();
        return false;
    }
    if (!document.is_object())
    {
        error_code = kErrManifestMalformed;
        message = std::string(kPackageManifestFileName) + "'s top level is not a JSON object";
        return false;
    }

    // DECISION 2 — the directory name and the manifest's own id must AGREE. Neither side is
    // authoritative alone: the directory name is what the `context-ext://` origin will be, and a
    // manifest that names a different id is either mispackaged or trying to have its bytes served
    // under another package's origin.
    const std::string declared_id = read_string(document, "id");
    if (declared_id != expected_package_id)
    {
        error_code = kErrManifestIdMismatch;
        message = std::string(kPackageManifestFileName) + " declares id '" + declared_id +
                  "' but its directory is named '" + expected_package_id + "'";
        return false;
    }

    if (!document.contains("contributions") || !document.at("contributions").is_array() ||
        document.at("contributions").size() == 0)
    {
        error_code = kErrManifestInvalid;
        message = "`contributions` is absent, is not an array, or is empty — a package that "
                  "contributes nothing has nothing to install";
        return false;
    }

    const Json& contributions = document.at("contributions");
    std::vector<gc::Contribution> parsed;
    // The final size is known EXACTLY (the loop fills all of them or returns false), and a
    // `Contribution` holds five strings plus two vectors, so each reallocation move-constructs every
    // one accepted so far. `package_mounts` below already reserves for the same reason.
    parsed.reserve(contributions.size());
    for (std::size_t index = 0; index < contributions.size(); ++index)
    {
        const Json& source = contributions.at(index);
        const std::string at = "contributions[" + std::to_string(index) + "]";
        if (!source.is_object())
        {
            error_code = kErrManifestInvalid;
            message = at + " is not an object";
            return false;
        }

        gc::Contribution contribution;
        contribution.id = read_string(source, "id");
        // (a) a non-empty, namespaced, unique id.
        if (contribution.id.empty())
        {
            error_code = kErrManifestInvalid;
            message = at + " carries no `id`";
            return false;
        }
        if (!id_is_namespaced_to(contribution.id, expected_package_id))
        {
            error_code = kErrManifestInvalid;
            message = at + "'s id '" + contribution.id + "' is not namespaced to the package: it "
                      "must be '" + expected_package_id + "' or start with '" + expected_package_id +
                      ".'";
            return false;
        }
        const bool duplicate =
            std::any_of(parsed.begin(), parsed.end(), [&contribution](const gc::Contribution& seen) {
                return seen.id == contribution.id;
            });
        if (duplicate)
        {
            error_code = kErrManifestInvalid;
            message = at + "'s id '" + contribution.id + "' is declared more than once";
            return false;
        }

        // (e) the contract major. Absent is ACCEPTED and means "the current one" — a manifest that
        // states nothing is not claiming an incompatible contract — but a stated value must match,
        // because the compatibility window is a single major (extension.h).
        if (source.contains("contractVersion"))
        {
            // Bounded to the i32 range the field casts to; an out-of-range or non-numeric value takes
            // the -1 fallback and so lands on the mismatch refusal below — fail-CLOSED.
            const std::int64_t stated = read_int(source, "contractVersion", -2147483648LL,
                                                 2147483647LL, -1);
            if (stated != static_cast<std::int64_t>(gc::kContractMajor))
            {
                error_code = kErrManifestInvalid;
                message = at + " declares contractVersion " + std::to_string(stated) +
                          "; this editor implements " + std::to_string(gc::kContractMajor);
                return false;
            }
        }
        contribution.contract_version = gc::kContractMajor;

        contribution.kind = read_kind(source);
        contribution.target = read_string(source, "target");
        contribution.title = read_string(source, "title", contribution.id);
        contribution.icon = read_string(source, "icon");

        const Json& dock = read_object(source, "dock");
        contribution.dock.default_zone = read_dock_zone(dock);
        // Clamped at 0 rather than refused: `DockDefaults` documents 0 as "no minimum stated" and
        // negatives as refused, so a negative arriving from a manifest becomes "unstated" — the
        // permissive-default half of the rule, since the cost is cosmetic.
        // BOUNDED TO THE i32 RANGE THE FIELD CASTS TO, and that bound is the correctness half rather
        // than tidiness: the clamp runs on the `int64`, so before the guard existed a manifest saying
        // `"minWidth": 4294967295` survived `std::max(0, …)` and then NARROWED to -1 — and
        // `registry.cpp` refuses a negative min size, so the scan would have reported as ACCEPTED a
        // package the registry then rejects, breaking this file's own promise (package_store.h § the
        // scan) that it never does that. Out of range now reads as "unstated" (0), the same permissive
        // direction a negative already took.
        contribution.dock.min_width =
            static_cast<int>(std::max<std::int64_t>(0, read_int(dock, "minWidth", -2147483648LL,
                                                               2147483647LL)));
        contribution.dock.min_height =
            static_cast<int>(std::max<std::int64_t>(0, read_int(dock, "minHeight", -2147483648LL,
                                                               2147483647LL)));

        // --- manifest v3 (04 §2) ------------------------------------------------------------------
        // (f) `instances`, `path`, `selection.subjects[]` and `events.{publishes,subscribes}[]`.
        //
        // The DECLARING PACKAGE ID is provenance, not manifest text: it is the directory name this
        // scan already agreed with the manifest's own `id` (decision 2 above), and it is what the
        // registry's namespacing rules are stated against. Set it before any v3 member is read so a
        // diagnostic can name it.
        contribution.package_id = expected_package_id;

        const Json& instances = read_object(source, "instances");
        if (instances.contains("mode"))
        {
            // FAILS CLOSED, unlike `dock.zone` above and for the reason that one states: an
            // unrecognised zone costs a panel its first position, while an unrecognised instance mode
            // would decide HOW MANY LIVE COPIES may exist. Defaulting that is not cosmetic, so the
            // token is refused rather than absorbed. Absent is still fine — `InstanceSpec`'s own
            // default is `singleton`, the restrictive answer, so a manifest stating nothing gets the
            // one mode that cannot surprise anybody.
            const std::string mode_token = read_string(instances, "mode");
            const gc::InstanceMode* matched = nullptr;
            for (const gc::InstanceMode& mode : gc::kInstanceModes)
            {
                if (mode_token == gc::instance_mode_token(mode))
                {
                    matched = &mode;
                    break;
                }
            }
            if (matched == nullptr)
            {
                error_code = kErrManifestInvalid;
                message = at + " declares instances.mode '" + mode_token +
                          "'; the vocabulary is closed (singleton | limited | unlimited)";
                return false;
            }
            contribution.instances.mode = *matched;
        }
        // Bounded to the i32 the field casts to, for the reason `minWidth` above states — an
        // out-of-range value reads as "unstated" (0), which the registry then refuses on `limited`
        // and accepts on the other two, exactly as an absent `max` would.
        contribution.instances.max = static_cast<int>(
            read_int(instances, "max", -2147483648LL, 2147483647LL));
        contribution.path = read_string(source, "path");

        std::string bad_entry;
        const Json& selection = read_object(source, "selection");
        if (!read_string_array(selection, "subjects", contribution.selection.subjects, bad_entry))
        {
            error_code = kErrManifestInvalid;
            message = at + "'s `selection.subjects` is malformed: " + bad_entry;
            return false;
        }
        const Json& events = read_object(source, "events");
        if (!read_string_array(events, "publishes", contribution.events.publishes, bad_entry))
        {
            error_code = kErrManifestInvalid;
            message = at + "'s `events.publishes` is malformed: " + bad_entry;
            return false;
        }
        if (!read_string_array(events, "subscribes", contribution.events.subscribes, bad_entry))
        {
            error_code = kErrManifestInvalid;
            message = at + "'s `events.subscribes` is malformed: " + bad_entry;
            return false;
        }

        // (b) content.type FAILS CLOSED, and only `iframe` is legal for a package. `uitree` and
        // `local` both mean "the editor renders this from its own code", which a third-party package
        // does not have — accepting either would be accepting a claim the package cannot back.
        const Json& content = read_object(source, "content");
        const std::string content_type = read_string(content, "type");
        // The accepted token comes from the FORWARD table too, so "what the editor emits" and "what this
        // parser accepts" cannot drift apart (see read_kind / read_dock_zone).
        if (content_type != gc::content_type_token(gc::ContentType::iframe))
        {
            error_code = kErrManifestInvalid;
            message = at + " declares content.type '" + content_type +
                      "'; a package contribution must be 'iframe' (the editor renders 'uitree' and "
                      "'local' panels from its own code)";
            return false;
        }
        contribution.content.type = gc::ContentType::iframe;
        contribution.content.entry = read_string(content, "entry");
        // (c) the entry must name THIS package's origin.
        if (!entry_is_own_origin(contribution.content.entry, expected_package_id))
        {
            error_code = kErrManifestInvalid;
            message = at + " declares content.entry '" + contribution.content.entry +
                      "'; a package panel's entry must be a " + std::string(kExtUrlPrefix) +
                      expected_package_id + "/… URL";
            return false;
        }

        const Json& state = read_object(source, "state");
        // The guard bounds here are deliberately WIDER than the u32 the field casts to, so that 0 and
        // negatives still REACH the `< 1` refusal below and it stays a live assertion instead of
        // becoming unreachable. Bounding at ±2^53 is what keeps read_int's own cast defined.
        const std::int64_t schema_version = read_int(state, gc::kStateSchemaVersionKey,
                                                    -9007199254740992LL, 9007199254740992LL, 1);
        if (schema_version < 1)
        {
            error_code = kErrManifestInvalid;
            message = at + " declares state." + std::string(gc::kStateSchemaVersionKey) + " " +
                      std::to_string(schema_version) + "; the D6 state contract starts at 1";
            return false;
        }
        // ⚠ THE UPPER BOUND IS ITS OWN REFUSAL, and it is not tidiness. `4294967296` is an EXACT
        // integer — no UB, and it sails past `>= 1` — and then NARROWS TO 0 on the cast below, which is
        // precisely the value `registry.cpp` refuses (`"state.schemaVersion is 0"`). Without this the
        // scan would report as ACCEPTED a package the registry then rejects, breaking this file's own
        // promise (package_store.h § the scan) that it never reports a package the Shell would refuse.
        if (schema_version > 4294967295LL)
        {
            error_code = kErrManifestInvalid;
            message = at + " declares state." + std::string(gc::kStateSchemaVersionKey) + " " +
                      std::to_string(schema_version) +
                      "; the D6 state contract stops at 4294967295";
            return false;
        }
        contribution.state.schema_version = static_cast<std::uint32_t>(schema_version);

        // (d) capabilities — deny-by-default over the CLOSED vocabulary. An unknown token is refused
        // rather than dropped: dropping one would present this package to e13c-4's consent surface as
        // asking for LESS than its manifest states.
        if (source.contains("capabilities"))
        {
            const Json& capabilities = source.at("capabilities");
            if (!capabilities.is_array())
            {
                error_code = kErrManifestInvalid;
                message = at + "'s `capabilities` is not an array";
                return false;
            }
            for (std::size_t c = 0; c < capabilities.size(); ++c)
            {
                const Json& token = capabilities.at(c);
                if (!token.is_string() || !gc::capability_supported(token.as_string()))
                {
                    error_code = kErrManifestInvalid;
                    message = at + " requests an unknown capability '" +
                              (token.is_string() ? token.as_string() : std::string("<non-string>")) +
                              "'";
                    return false;
                }
                contribution.capabilities.push_back(token.as_string());
            }
        }

        // The manifest-declared commands. TOTAL and permissive, matching `readManifestCommands`:
        // non-object entries and entries with no usable id are DROPPED rather than refusing the whole
        // package — a command the editor cannot name is simply not offered, which costs the package a
        // menu entry and costs the editor nothing. A NAMESPACE rule is deliberately NOT applied here:
        // command ids are resolved by the e07b registry against its own duplicate rules, and e13c-4
        // owns which of a package's commands are registered at all.
        if (source.contains("commands") && source.at("commands").is_array())
        {
            const Json& commands = source.at("commands");
            for (std::size_t c = 0; c < commands.size(); ++c)
            {
                const Json& entry = commands.at(c);
                if (!entry.is_object())
                {
                    continue;
                }
                const std::string id = read_string(entry, "id");
                if (id.empty())
                {
                    continue;
                }
                contribution.commands.push_back(
                    gc::CommandContribution{id, read_string(entry, "title", id),
                                            read_string(entry, "when")});
            }
        }

        // `sandbox` is LEFT AT ITS DEFAULT — least privilege — and any grant the manifest states is
        // IGNORED rather than read. See package_store.h: `capabilities` is what a package ASKS for;
        // what it is GIVEN comes from e13c-4's consent surface, and a reader that honoured a
        // self-declared grant would be the whole capability model bypassed by one JSON member.
        // `themes` is likewise not read here (e06's package-theme validation is its own gate).

        // (g) THE REGISTRY'S OWN STRUCTURAL VERDICT, ASKED RATHER THAN RE-IMPLEMENTED. This file
        // promises (package_store.h § the scan) never to report as ACCEPTED a package the registry
        // would then refuse, and that promise has already been broken twice by arithmetic — an
        // oversized `minWidth` narrowing to -1, an oversized `schemaVersion` narrowing to 0 — each
        // fixed by a bespoke guard bolted on beside the parse. Manifest v3 adds five more members
        // with structural rules (instances.mode/max coherence, `path` display form, and the D2/D4
        // namespacing of subjects and topics), so a sixth bespoke guard would be a second opinion
        // free to drift from the one that actually decides. Asking `gc::manifest_defect` — the SAME
        // function `register_contribution` refuses on — makes the promise structurally true instead
        // of maintained by hand. Run LAST so every rule above keeps its own, more specific
        // diagnostic; this one only answers for what those did not check.
        if (const std::string defect = gc::manifest_defect(contribution); !defect.empty())
        {
            error_code = kErrManifestInvalid;
            message = at + " has an invalid manifest: " + defect;
            return false;
        }
        parsed.push_back(std::move(contribution));
    }

    out.id = expected_package_id;
    out.root = package_root;
    out.version = read_string(document, "version");
    out.contributions = std::move(parsed);
    return true;
}

// -------------------------------------------------------------------------------------- the scan

PackageStoreScan scan_package_store(const fs::path& store_root)
{
    PackageStoreScan scan;
    if (store_root.empty())
    {
        // No home directory. Not a fault, and not reported as one — the editor simply has no store.
        return scan;
    }

    std::error_code ec;
    if (!fs::is_directory(store_root, ec))
    {
        // ⚠ `ec` DECIDES WHICH FAULT THIS IS, and reading it is the point. "Not a directory" has two
        // very different causes: the store has not been created yet (a first-run machine — reported so
        // that "no third-party panels" always has a stated reason, but an ordinary state), or the store
        // could not be EXAMINED at all because something ABOVE it is unreadable (a mode-000 `~/.context`
        // from a restored backup or a hardening mistake). Reporting the second as "does not exist yet" —
        // a code this file documents as "NOT an error" — tells a user whose packages are all present
        // that they have none, which is exactly the diagnosis-defeating silence decision 3 forbids. The
        // enumeration below already distinguishes the same two cases one level down; this is that fix
        // applied one level UP.
        // ⚠ A TRUTHY `ec` IS NOT BY ITSELF "UNREADABLE" — the ERRNO VALUE is the discriminator.
        // MEASURED: libc++ sets `ec` to ENOENT from `is_directory` for a path that simply does not
        // exist, so testing `ec` alone reported every first-run machine as an unreadable store. The
        // suite's pre-existing `kErrPackageStoreAbsent` assertion is what caught that.
        const bool unreadable = ec && ec != std::errc::no_such_file_or_directory &&
                                ec != std::errc::not_a_directory;
        scan.refusals.push_back(PackageRefusal{
            "", store_root, unreadable ? kErrPackageStoreUnreadable : kErrPackageStoreAbsent,
            unreadable ? "the package store could not be examined: " + ec.message()
                       : "the package store does not exist yet, so no packages are installed"});
        return scan;
    }

    // Sorted for DETERMINISM. `directory_iterator` order is filesystem-defined, and the mount table
    // it feeds is order-sensitive in one visible way — `ExtAssetResolver::mount` refuses the SECOND
    // of any overlapping pair, so an unstable order would make WHICH package is refused vary run to
    // run. A store whose diagnostics change without the disk changing is not diagnosable.
    // NO `skip_permission_denied`, deliberately. With that option an unreadable store yields end()
    // with NO error, so the scan would report an EMPTY store rather than an unreadable one — a silent
    // drop of exactly the kind decision 3 exists to forbid, and the one a user is least able to
    // diagnose (their packages are installed and simply absent). Letting the error surface is what
    // makes `kErrPackageStoreUnreadable` reachable at all.
    std::vector<fs::path> entries;
    ec.clear();
    for (fs::directory_iterator it(store_root, ec); !ec && it != fs::directory_iterator();
         it.increment(ec))
    {
        if (entries.size() >= kMaxPackageStoreEntries)
        {
            scan.refusals.push_back(PackageRefusal{
                "", store_root, kErrPackageStoreTooManyEntries,
                "the package store holds more than " + std::to_string(kMaxPackageStoreEntries) +
                    " entries; the remainder was not considered"});
            break;
        }
        entries.push_back(it->path());
    }
    if (ec)
    {
        scan.refusals.push_back(PackageRefusal{"", store_root, kErrPackageStoreUnreadable,
                                               "the package store could not be enumerated: " +
                                                   ec.message()});
        return scan;
    }
    std::sort(entries.begin(), entries.end());

    for (const fs::path& entry : entries)
    {
        const std::string name = entry.filename().string();
        // A NON-DIRECTORY entry is skipped SILENTLY, and it is the one silence in this function: a
        // store directory legitimately accumulates sibling files (a download's leftover archive, a
        // `.DS_Store`), none of which is a package claiming to be installed. Reporting each as a
        // refusal would bury the refusals that mean something.
        ec.clear();
        if (!fs::is_directory(entry, ec))
        {
            continue;
        }

        // ⚠ THIS IS ALSO THE SECOND E13B OBLIGATION'S WHOLE DISCHARGE — see package_store.h § the
        // scan. A valid id is lower-case ONLY, so a directory named `Pkg` on a case-insensitive NTFS
        // volume lands HERE, reported, rather than becoming a second spelling of `pkg` that the
        // mount's case-SENSITIVE overlap comparison could not see. No downstream dedupe follows,
        // because after this refusal two accepted ids cannot differ by case at all.
        if (!is_valid_package_id(name))
        {
            scan.refusals.push_back(PackageRefusal{
                name, entry, kErrPackageIdInvalid,
                "'" + name + "' is not a valid package id, so it could never be named by a " +
                    std::string(kExtUrlPrefix) + " request"});
            continue;
        }

        // PROVENANCE, checked here as well as at the mount — see package_store.h: a scan that
        // reported a package the Shell then refused would be reporting something untrue. The root is
        // BUILT from the store root and the enumerated name, never read from the manifest, so this
        // can only fail on a real filesystem fact (the entry is a symlink / junction, or it resolves
        // outside the store).
        fs::path canonical;
        std::string provenance_code;
        std::string provenance_message;
        if (!package_root_provenance_ok(store_root, entry, canonical, provenance_code,
                                       provenance_message))
        {
            scan.refusals.push_back(
                PackageRefusal{name, entry, provenance_code, provenance_message});
            continue;
        }

        InstalledPackage package;
        std::string manifest_code;
        std::string manifest_message;
        if (!read_package_manifest(canonical / kPackageManifestFileName, name, canonical, package,
                                   manifest_code, manifest_message))
        {
            scan.refusals.push_back(PackageRefusal{name, entry, manifest_code, manifest_message});
            continue;
        }

        scan.packages.push_back(std::move(package));
    }

    return scan;
}

std::vector<ExtPackageMount> package_mounts(const PackageStoreScan& scan)
{
    std::vector<ExtPackageMount> mounts;
    mounts.reserve(scan.packages.size());
    for (const InstalledPackage& package : scan.packages)
    {
        mounts.push_back(ExtPackageMount{package.id, package.root});
    }
    return mounts;
}

} // namespace context::editor::shell
