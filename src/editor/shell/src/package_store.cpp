// The package store — `<home>/.context/packages` enumeration, the manifest -> `Contribution` mapping,
// and the mount projection. See package_store.h for the three decisions, the manifest shape and the
// five validation rules; see ext_scheme.h § mount PROVENANCE for the containment check every root
// here has already passed.

#include "context/editor/shell/package_store.h"

#include "context/editor/contract/json.h"
#include "context/editor/gui/contract/panel_state.h" // kStateSchemaVersionKey — the D6 state key
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
// themes_bridge.cpp / keybindings_bridge.cpp; not shared because each carries its OWN cap, and a
// shared helper with a caller-supplied cap is the shape in which one caller's cap silently becomes
// every caller's.
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

// --- the permissive readers (mirroring panels.ts's `readString` / `readNumber` / `readBoolean`) ----

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

[[nodiscard]] bool read_bool(const Json& source, const char* key)
{
    return source.is_object() && source.contains(key) && source.at(key).is_bool() &&
           source.at(key).as_bool();
}

[[nodiscard]] std::int64_t read_int(const Json& source, const char* key, std::int64_t fallback = 0)
{
    if (!source.is_object() || !source.contains(key))
    {
        return fallback;
    }
    const Json& value = source.at(key);
    return value.is_number() ? value.as_int() : fallback;
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

// The manifest's `dock.zone` token -> DockZone. An unrecognised zone falls back to `center`, exactly
// as `readDock` in panels.ts does: the vocabulary is closed, so anything else is drift rather than a
// new zone, and the cost of the fallback is cosmetic (where a panel first appears). Contrast
// `content.type` below, which fails CLOSED because the cost there is not cosmetic.
[[nodiscard]] gc::DockZone read_dock_zone(const Json& dock)
{
    const std::string token = read_string(dock, "zone", "center");
    if (token == "left")
    {
        return gc::DockZone::left;
    }
    if (token == "right")
    {
        return gc::DockZone::right;
    }
    if (token == "top")
    {
        return gc::DockZone::top;
    }
    if (token == "bottom")
    {
        return gc::DockZone::bottom;
    }
    return gc::DockZone::center;
}

// The manifest's `kind` token -> ContributionKind, falling back to `panel`. A package contributing an
// inspector or a gizmo is a designed part of the R-EDIT-001 contract (extension.h), so the vocabulary
// is read rather than pinned to panels — but an unrecognised token becomes `panel`, the kind with no
// `target` semantics, rather than being trusted into a target-keyed lookup.
[[nodiscard]] gc::ContributionKind read_kind(const Json& source)
{
    const std::string token = read_string(source, "kind", "panel");
    if (token == "inspector")
    {
        return gc::ContributionKind::inspector;
    }
    if (token == "gizmo")
    {
        return gc::ContributionKind::gizmo;
    }
    if (token == "asset_kind_editor")
    {
        return gc::ContributionKind::asset_kind_editor;
    }
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

    std::error_code ec;
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
            const std::int64_t stated = read_int(source, "contractVersion", -1);
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
        contribution.dock.singleton = read_bool(dock, "singleton");
        // Clamped at 0 rather than refused: `DockDefaults` documents 0 as "no minimum stated" and
        // negatives as refused, so a negative arriving from a manifest becomes "unstated" — the
        // permissive-default half of the rule, since the cost is cosmetic.
        contribution.dock.min_width = static_cast<int>(std::max<std::int64_t>(0, read_int(dock, "minWidth")));
        contribution.dock.min_height =
            static_cast<int>(std::max<std::int64_t>(0, read_int(dock, "minHeight")));

        // (b) content.type FAILS CLOSED, and only `iframe` is legal for a package. `uitree` and
        // `local` both mean "the editor renders this from its own code", which a third-party package
        // does not have — accepting either would be accepting a claim the package cannot back.
        const Json& content = read_object(source, "content");
        const std::string content_type = read_string(content, "type");
        if (content_type != "iframe")
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
        const std::int64_t schema_version = read_int(state, gc::kStateSchemaVersionKey, 1);
        if (schema_version < 1)
        {
            error_code = kErrManifestInvalid;
            message = at + " declares state." + std::string(gc::kStateSchemaVersionKey) + " " +
                      std::to_string(schema_version) + "; the D6 state contract starts at 1";
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
        // A first-run machine. REPORTED rather than silent so "no third-party panels" always has a
        // stated reason, but it is an ordinary state and not an error the user can act on.
        scan.refusals.push_back(PackageRefusal{
            "", store_root, kErrPackageStoreAbsent,
            "the package store does not exist yet, so no packages are installed"});
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
