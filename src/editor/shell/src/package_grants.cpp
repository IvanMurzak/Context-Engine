// The persisted per-package grant store + the install-consent surface (see package_grants.h).

#include "context/editor/shell/package_grants.h"

#include "context/editor/shell/ext_scheme.h"
#include "context/editor/shell/keybindings_bridge.h"
#include "context/editor/shell/user_config.h"

#include <algorithm>
#include <optional>
#include <utility>

namespace context::editor::shell
{

namespace gc = gui::contract;

namespace
{

// The closed manifest capability vocabulary, in ONE order.
//
// Spelled out here rather than derived, and paired with `capability_supported` by the assertion in
// this module's suite: the vocabulary lives in `extension.h` and a token added there without being
// added here would be accepted by `capability_supported` and silently unrepresentable in a grant —
// i.e. a capability an operator could consent to and a package could never hold.
constexpr const char* kCapabilityVocabulary[] = {
    gc::kCapabilityReadQuery,
    gc::kCapabilityFileWrite,
    gc::kCapabilitySessionControl,
    gc::kCapabilityBuildInstall,
    gc::kCapabilityUiEvents,
};

// The daemon scope a manifest capability confers, or nullopt when it confers NONE.
//
// `read_query` returns nullopt because it is the baseline every `ScopeSet` holds by construction, and
// `ui_events` returns nullopt because it is an editor-core-LOCAL grant over the `editor.ui` bus with
// no daemon meaning at all — which is what makes granting it structurally unable to widen a package's
// daemon session (package_grants.h § granted_scope_set).
std::optional<bridge::Scope> scope_for_capability(const std::string& capability)
{
    if (capability == gc::kCapabilityFileWrite)
    {
        return bridge::Scope::file_write;
    }
    if (capability == gc::kCapabilitySessionControl)
    {
        return bridge::Scope::session_control;
    }
    if (capability == gc::kCapabilityBuildInstall)
    {
        return bridge::Scope::build_install;
    }
    return std::nullopt;
}

bool contains_token(const std::vector<std::string>& tokens, const std::string& token)
{
    return std::find(tokens.begin(), tokens.end(), token) != tokens.end();
}

// Re-order `tokens` into the closed vocabulary's own order, dropping duplicates and anything outside
// it. Every list this module hands out or stores goes through here, so no caller can observe an order
// that depends on how a document happened to be written.
std::vector<std::string> canonical_capabilities(const std::vector<std::string>& tokens)
{
    std::vector<std::string> out;
    for (const char* known : kCapabilityVocabulary)
    {
        if (contains_token(tokens, known))
        {
            out.emplace_back(known);
        }
    }
    return out;
}

// Read one package's recorded array. Returns false — and records the fault — when the entry is not an
// array of strings, so the whole PACKAGE is dropped rather than half-honoured (decision 2).
bool read_entry(const std::string& package_id, const contract::Json& value,
                std::vector<std::string>& out, std::vector<GrantDiagnostic>& diagnostics)
{
    if (!value.is_array())
    {
        diagnostics.push_back({package_id, kErrGrantsEntryInvalid,
                               "the recorded grant for '" + package_id +
                                   "' is not an array of capability tokens; it grants nothing"});
        return false;
    }
    std::vector<std::string> tokens;
    for (std::size_t i = 0; i < value.size(); ++i)
    {
        const contract::Json& item = value.at(i);
        if (!item.is_string())
        {
            diagnostics.push_back({package_id, kErrGrantsEntryInvalid,
                                   "the recorded grant for '" + package_id +
                                       "' holds a non-string entry; it grants nothing"});
            return false;
        }
        if (!gc::capability_supported(item.as_string()))
        {
            // The TOKEN is dropped, not the package: an engine that widened its vocabulary and then
            // narrowed it again would otherwise revoke every grant on the same document, and the
            // surviving tokens were each consented to on their own.
            //
            // ⚠ THE VALUE-LEVEL PROTECTION HERE IS REDUNDANT, AND SAYING SO IS THE POINT (MEASURED by
            // this task's plant round, plant P02): `canonical_capabilities` below re-orders through
            // `kCapabilityVocabulary` and therefore ALSO drops anything outside it, so deleting this
            // guard does NOT change what any caller reads — the suite's exact-list assertion cannot
            // fail on it, and a plant that only removed this branch would have scored a misleading
            // GREEN on that half. What this branch UNIQUELY contributes is the NAMED DIAGNOSTIC: the
            // operator learns WHICH token was ignored, where silent canonicalization would leave a
            // stale grants file looking honoured. That is what the suite pins, and it is why this is
            // defence-in-depth rather than the control.
            diagnostics.push_back({package_id, kErrGrantsUnknownCapability,
                                   "the recorded grant for '" + package_id + "' names the unknown "
                                   "capability '" + item.as_string() + "'; that token is dropped"});
            continue;
        }
        tokens.push_back(item.as_string());
    }
    out = canonical_capabilities(tokens);
    return true;
}

} // namespace

// ---------------------------------------------------------------------------- the document on disk

std::filesystem::path package_grants_path()
{
    const std::optional<std::filesystem::path> home = home_directory();
    if (!home.has_value())
    {
        return {};
    }
    return *home / ".context" / kPackageGrantsFileName;
}

// ------------------------------------------------------------------------- capabilities -> scopes

bridge::ScopeSet granted_scope_set(const std::vector<std::string>& capabilities)
{
    bridge::ScopeSet scopes; // the read_query baseline, held by construction
    for (const std::string& capability : capabilities)
    {
        if (const std::optional<bridge::Scope> scope = scope_for_capability(capability))
        {
            scopes.grant(*scope);
        }
    }
    return scopes;
}

std::string attach_scope_spec(const bridge::ScopeSet& scopes)
{
    // BASELINE FIRST and always present. `ScopeSet::parse` treats "read" as the no-op baseline token,
    // so it carries no grant — but an EMPTY spec would leave `AttachOptions::scope` reading as
    // "nothing was decided" where the shipped constant it replaces reads `"read"`, and the whole point
    // of spelling that constant out (package_sessions.h control 1) was that a default is not a
    // decision anyone can find.
    std::string spec = "read";
    if (scopes.has(bridge::Scope::file_write))
    {
        spec += ",";
        spec += gc::kCapabilityFileWrite;
    }
    if (scopes.has(bridge::Scope::session_control))
    {
        spec += ",";
        spec += gc::kCapabilitySessionControl;
    }
    if (scopes.has(bridge::Scope::build_install))
    {
        spec += ",";
        spec += gc::kCapabilityBuildInstall;
    }
    return spec;
}

std::vector<std::string> clamp_to_declared(const std::vector<std::string>& granted,
                                           const std::vector<std::string>& declared)
{
    std::vector<std::string> kept;
    for (const std::string& capability : granted)
    {
        if (gc::capability_supported(capability) && contains_token(declared, capability))
        {
            kept.push_back(capability);
        }
    }
    return canonical_capabilities(kept);
}

// ---------------------------------------------------------------------------------- the store

const PackageGrantStore::Entry* PackageGrantStore::find(const std::string& package_id) const
{
    const auto it = std::find_if(entries_.begin(), entries_.end(), [&](const Entry& entry)
                                 { return entry.package_id == package_id; });
    return it == entries_.end() ? nullptr : &*it;
}

PackageGrantStore PackageGrantStore::load(const std::filesystem::path& file,
                                          std::vector<GrantDiagnostic>& diagnostics)
{
    PackageGrantStore store;
    if (file.empty())
    {
        // No home directory. NOT a diagnostic — see the header: it is not a fault an operator can act
        // on, and every package simply runs at the deny-all baseline.
        return store;
    }
    std::error_code ec;
    if (!std::filesystem::exists(file, ec) || ec)
    {
        diagnostics.push_back({{}, kErrGrantsAbsent,
                               "no grant document at " + file.string() +
                                   "; every package holds the deny-all baseline"});
        return store;
    }
    // THE SHELL'S ONE BOUNDED READER, reused rather than re-derived: it caps the read at
    // `kMaxUserConfigBytes`, and an absent / unreadable / oversized / malformed / non-object file all
    // arrive here as an EMPTY object — which is exactly this module's deny-all default. Its name says
    // `user_config` because that was its first caller; its contract is a bounded JSON-object read of a
    // path (user_config.h), and a second copy of it would be a second cap to drift.
    const contract::Json document = read_user_config(file);
    if (!document.contains("version") || document.at("version").as_int() != kPackageGrantsVersion)
    {
        diagnostics.push_back({{}, kErrGrantsMalformed,
                               "the grant document at " + file.string() +
                                   " is malformed or carries an unrecognised version; every package "
                                   "holds the deny-all baseline"});
        return store;
    }
    if (!document.contains("grants") || !document.at("grants").is_object())
    {
        diagnostics.push_back({{}, kErrGrantsMalformed,
                               "the grant document at " + file.string() +
                                   " has no 'grants' object; every package holds the deny-all "
                                   "baseline"});
        return store;
    }
    for (const std::pair<std::string, contract::Json>& member :
         document.at("grants").object_members())
    {
        if (!is_valid_package_id(member.first))
        {
            // The SAME predicate the scheme mounts against and the session table validates: a grant
            // recorded under a name no package can ever have is unreachable by construction, so
            // keeping it would only make the document read as granting more than it does.
            diagnostics.push_back({member.first, kErrGrantsEntryInvalid,
                                   "'" + member.first +
                                       "' is not a valid package id; its recorded grant is ignored"});
            continue;
        }
        std::vector<std::string> capabilities;
        if (!read_entry(member.first, member.second, capabilities, diagnostics))
        {
            continue;
        }
        store.entries_.push_back(Entry{member.first, std::move(capabilities)});
    }
    return store;
}

bool PackageGrantStore::granted(const std::string& package_id, const std::string& capability) const
{
    const Entry* entry = find(package_id);
    return entry != nullptr && contains_token(entry->capabilities, capability);
}

std::vector<std::string> PackageGrantStore::granted_capabilities(const std::string& package_id) const
{
    const Entry* entry = find(package_id);
    return entry == nullptr ? std::vector<std::string>{} : entry->capabilities;
}

bool PackageGrantStore::decided(const std::string& package_id) const
{
    return find(package_id) != nullptr;
}

void PackageGrantStore::record(const std::string& package_id,
                               const std::vector<std::string>& capabilities,
                               std::vector<GrantDiagnostic>& diagnostics)
{
    std::vector<std::string> accepted;
    for (const std::string& capability : capabilities)
    {
        if (!gc::capability_supported(capability))
        {
            diagnostics.push_back({package_id, kErrGrantsUnknownCapability,
                                   "'" + capability +
                                       "' is not a known capability; it is not granted to '" +
                                       package_id + "'"});
            continue;
        }
        accepted.push_back(capability);
    }
    std::vector<std::string> canonical = canonical_capabilities(accepted);
    const auto it = std::find_if(entries_.begin(), entries_.end(), [&](const Entry& entry)
                                 { return entry.package_id == package_id; });
    if (it == entries_.end())
    {
        entries_.push_back(Entry{package_id, std::move(canonical)});
        return;
    }
    // REPLACED WHOLE, never merged — see the header: a consent surface that could only add would make
    // a revocation unexpressible, and "the operator narrowed this package's grant" is the answer that
    // matters most here.
    it->capabilities = std::move(canonical);
}

bool PackageGrantStore::save(const std::filesystem::path& file, std::string& error) const
{
    if (file.empty())
    {
        error = "no home directory: there is nowhere to record a consent decision";
        return false;
    }
    std::vector<const Entry*> ordered;
    ordered.reserve(entries_.size());
    for (const Entry& entry : entries_)
    {
        ordered.push_back(&entry);
    }
    // SORTED so two stores holding the same grants produce byte-identical documents — a security
    // document whose diff is dominated by reordering is one nobody reviews.
    std::sort(ordered.begin(), ordered.end(),
              [](const Entry* a, const Entry* b) { return a->package_id < b->package_id; });

    contract::Json grants = contract::Json::object();
    for (const Entry* entry : ordered)
    {
        contract::Json tokens = contract::Json::array();
        // Already canonical (every writer goes through `canonical_capabilities`), so this is the
        // vocabulary's order rather than a second sort with its own opinion.
        for (const std::string& capability : entry->capabilities)
        {
            tokens.push_back(contract::Json(capability));
        }
        grants.set(entry->package_id, std::move(tokens));
    }
    contract::Json document = contract::Json::object();
    document.set("version", contract::Json(static_cast<std::int64_t>(kPackageGrantsVersion)));
    document.set("grants", std::move(grants));

    // THE SHELL'S ONE ATOMIC WRITE PRIMITIVE (user_config.h): create-parent, stage to a UNIQUE temp
    // name, rename over. A half-written grant document is the failure mode that would silently widen
    // or narrow a package's authority on the next boot.
    std::string write_error;
    if (!write_user_config(file, document, &write_error))
    {
        error = write_error.empty() ? "the grant document could not be written" : write_error;
        return false;
    }
    return true;
}

// -------------------------------------------------------------------------- the consent surface

std::vector<PackageConsentRequest> package_consent_requests(const PackageStoreScan& scan,
                                                            const PackageGrantStore& grants)
{
    std::vector<PackageConsentRequest> requests;
    requests.reserve(scan.packages.size());
    for (const InstalledPackage& package : scan.packages)
    {
        std::vector<std::string> requested;
        for (const gc::Contribution& contribution : package.contributions)
        {
            for (const std::string& capability : contribution.capabilities)
            {
                if (!contains_token(requested, capability))
                {
                    requested.push_back(capability);
                }
            }
        }
        requested = canonical_capabilities(requested);
        PackageConsentRequest request;
        request.id = package.id;
        request.version = package.version;
        request.granted = clamp_to_declared(grants.granted_capabilities(package.id), requested);
        request.requested = std::move(requested);
        request.decided = grants.decided(package.id);
        requests.push_back(std::move(request));
    }
    return requests;
}

std::vector<PackageConsentRequest> pending_consent_requests(const PackageStoreScan& scan,
                                                            const PackageGrantStore& grants)
{
    std::vector<PackageConsentRequest> pending;
    for (PackageConsentRequest& request : package_consent_requests(scan, grants))
    {
        if (!request.decided && !request.requested.empty())
        {
            pending.push_back(std::move(request));
        }
    }
    return pending;
}

std::string consent_prompt_line(const PackageConsentRequest& request)
{
    std::string line = "package '" + request.id + "'";
    if (!request.version.empty())
    {
        line += " v" + request.version;
    }
    line += " requests: ";
    if (request.requested.empty())
    {
        line += "(nothing beyond the read-only baseline)";
    }
    else
    {
        for (std::size_t i = 0; i < request.requested.size(); ++i)
        {
            line += (i == 0 ? "" : ", ") + request.requested[i];
        }
    }
    if (!request.decided)
    {
        line += " — NOT YET CONSENTED (it holds the deny-all baseline)";
        return line;
    }
    line += " — granted: ";
    if (request.granted.empty())
    {
        line += "(none)";
        return line;
    }
    for (std::size_t i = 0; i < request.granted.size(); ++i)
    {
        line += (i == 0 ? "" : ", ") + request.granted[i];
    }
    return line;
}

void apply_package_grants(PackageStoreScan& scan, const PackageGrantStore& grants)
{
    for (InstalledPackage& package : scan.packages)
    {
        const std::vector<std::string> held = grants.granted_capabilities(package.id);
        for (gc::Contribution& contribution : package.contributions)
        {
            // CLAMPED PER CONTRIBUTION, against ITS OWN declaration — not against the package's union.
            // A package whose panel A declares `file_write` and whose panel B does not must not have B
            // silently ride A's grant: `manifest_defect` judges each contribution alone, and a store
            // path that produced a union grant would red the registry for B while looking correct
            // here.
            contribution.sandbox.granted_scopes =
                granted_scope_set(clamp_to_declared(held, contribution.capabilities));
        }
    }
}

// ------------------------------------------------------------------------------------- the host

PackageGrantHost::PackageGrantHost(PackageStoreScan& scan, std::filesystem::path grants_file)
    : scan_(scan), grants_file_(std::move(grants_file))
{
    // ⚠ THE LOAD RUNS IN THE BODY, NOT IN THE MEMBER-INITIALIZER LIST, AND THAT IS DELIBERATE. It
    // writes into `diagnostics_`, and a member initializer would run at whatever point `grants_`'
    // DECLARATION order puts it — which was ahead of `diagnostics_`' own constructor, i.e. a
    // `push_back` into a vector that did not exist yet (see the header: UB the `dev` build survived
    // and ASan aborted on). In the body every member is already constructed, so the hazard is
    // structural rather than a comment asking the next editor to preserve a field order.
    grants_ = PackageGrantStore::load(grants_file_, diagnostics_);
    // THE GRANTS ARE APPLIED AT CONSTRUCTION, not left to the caller. `Contribution::sandbox` arrives
    // from `read_package_manifest` at least privilege for every package (package_store.h), so a host
    // that loaded the document but did not apply it would leave a granted package silently denied —
    // failing in the safe direction, and therefore in the direction nobody notices.
    apply_package_grants(scan_, grants_);
}

BridgeResult PackageGrantHost::list() const
{
    contract::Json packages = contract::Json::array();
    for (const PackageConsentRequest& request : package_consent_requests(scan_, grants_))
    {
        contract::Json entry = contract::Json::object();
        entry.set("id", contract::Json(request.id));
        entry.set("version", contract::Json(request.version));
        contract::Json requested = contract::Json::array();
        for (const std::string& capability : request.requested)
        {
            requested.push_back(contract::Json(capability));
        }
        contract::Json granted = contract::Json::array();
        for (const std::string& capability : request.granted)
        {
            granted.push_back(contract::Json(capability));
        }
        entry.set("requested", std::move(requested));
        entry.set("granted", std::move(granted));
        entry.set("decided", contract::Json(request.decided));
        packages.push_back(std::move(entry));
    }
    contract::Json result = contract::Json::object();
    result.set("packages", std::move(packages));
    return BridgeResult::ok(std::move(result));
}

BridgeResult PackageGrantHost::decide(const std::string& package_id,
                                      const std::vector<std::string>& capabilities)
{
    if (!is_valid_package_id(package_id))
    {
        return BridgeResult::error(kErrGrantsBadParams,
                                   "package.grants.decide requires a valid 'packageId'");
    }
    // THE DECISION IS SCOPED TO WHAT IS INSTALLED. A grant recorded for a package the scan did not
    // accept could never be enforced against anything — but it WOULD sit in the document waiting for
    // a directory of that name to appear, which turns the consent record into a pre-authorization.
    const auto installed = std::find_if(scan_.packages.begin(), scan_.packages.end(),
                                        [&](const InstalledPackage& package)
                                        { return package.id == package_id; });
    if (installed == scan_.packages.end())
    {
        return BridgeResult::error(kErrGrantsUnknownPackage,
                                   "no installed package '" + package_id + "' to consent to");
    }
    // CLAMPED TO THE MANIFEST BEFORE IT IS RECORDED (decision 3): a consent surface that recorded more
    // than the manifest declared would hand the registry a contribution `manifest_defect` refuses, and
    // the user would see their answer turn a working package into a broken one.
    std::vector<std::string> declared;
    for (const gc::Contribution& contribution : installed->contributions)
    {
        for (const std::string& capability : contribution.capabilities)
        {
            if (!contains_token(declared, capability))
            {
                declared.push_back(capability);
            }
        }
    }
    grants_.record(package_id, clamp_to_declared(capabilities, declared), diagnostics_);
    ++decisions_recorded_;
    apply_package_grants(scan_, grants_);

    std::string error;
    if (!grants_.save(grants_file_, error))
    {
        // REPORTED, NOT SWALLOWED — and the in-memory decision STANDS for this session. Reverting it
        // would mean an operator who answered on a read-only home directory gets a package that is
        // denied AND a dialog that keeps re-asking; reporting it means the answer works now and the
        // reason it will not survive a restart is on the record.
        diagnostics_.push_back({package_id, kErrGrantsWriteFailed, error});
        return BridgeResult::error(kErrGrantsWriteFailed, error);
    }
    return list();
}

bool PackageGrantHost::install(BridgeRouter& router)
{
    if (!router.register_method(kPackageGrantsListMethod,
                                [this](const BridgeRequest&) -> BridgeResult { return list(); }))
    {
        return false;
    }
    return router.register_method(
        kPackageGrantsDecideMethod,
        [this](const BridgeRequest& request) -> BridgeResult
        {
            if (!request.params.contains("packageId") ||
                !request.params.at("packageId").is_string())
            {
                return BridgeResult::error(kErrGrantsBadParams,
                                           "package.grants.decide requires a string 'packageId'");
            }
            const contract::Json& raw = request.params.at("capabilities");
            if (!raw.is_array())
            {
                // FAIL-CLOSED ON AN ABSENT MEMBER TOO. Defaulting a missing `capabilities` to "grant
                // nothing" would be safe, but it would also make a renderer bug that dropped the
                // member look exactly like a deliberate refusal — and this is the one route whose
                // whole job is to record what a human actually said.
                return BridgeResult::error(kErrGrantsBadParams,
                                           "package.grants.decide requires a 'capabilities' array");
            }
            std::vector<std::string> capabilities;
            for (std::size_t i = 0; i < raw.size(); ++i)
            {
                if (!raw.at(i).is_string())
                {
                    return BridgeResult::error(
                        kErrGrantsBadParams,
                        "package.grants.decide 'capabilities' must hold strings only");
                }
                capabilities.push_back(raw.at(i).as_string());
            }
            return decide(request.params.at("packageId").as_string(), capabilities);
        });
}

} // namespace context::editor::shell
