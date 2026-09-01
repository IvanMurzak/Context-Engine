// The package fact bus, the Shell's half (see package_facts.h).

#include "context/editor/shell/package_facts.h"

#include "context/editor/gui/contract/extension.h"
#include "context/editor/shell/ext_scheme.h"

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

namespace context::editor::shell
{

namespace gc = gui::contract;

namespace
{

// Read a required string member off a params object. Mirrors package_sessions.cpp's helper — the
// params here arrive on the SAME untrusted-renderer channel and get the same discipline.
[[nodiscard]] bool read_string(const contract::Json& params, const std::string& key,
                               std::string& out)
{
    if (!params.is_object() || !params.contains(key))
    {
        return false;
    }
    const contract::Json& value = params.at(key);
    if (!value.is_string())
    {
        return false;
    }
    out = value.as_string();
    return true;
}

[[nodiscard]] bool contains_name(const std::vector<std::string>& names, const std::string& name)
{
    return std::find(names.begin(), names.end(), name) != names.end();
}

// Is `topic` a real SUB-name of `owner` — `<owner>.<something>`, never the bare owner id?
//
// BOTH HALVES MATTER, and they are the halves `registry.cpp`'s `is_namespaced_under` and
// `validatePackageTopic` (uibus.ts) already state: the bare package id is a NAMESPACE, not a member
// of it, so accepting it would let one package's topic and its id be the same string — and a
// consumer could then not tell "the package" from "a fact of the package" by reading the name.
[[nodiscard]] bool is_namespaced_under(const std::string& topic, const std::string& owner)
{
    return topic.size() > owner.size() + 1 && topic.compare(0, owner.size(), owner) == 0 &&
           topic[owner.size()] == '.';
}

// The union of one member list across a package's contributions, deduplicated, in declaration order.
//
// ⚠ THE UNION IS PER PACKAGE, NOT PER CONTRIBUTION, and that is the same shape (and the same stated
// limitation) as `declared_capabilities` in package_grants.cpp: the grant document is keyed by
// package, the daemon session is pooled by package, and the Shell's delivery buffer is per package —
// so a per-contribution answer would be a distinction no layer below could act on. A package whose
// panel A declares a topic can therefore publish it from panel B. That is honest rather than
// accidental: both panels are the same trust principal, loaded from the same root, over the same
// session.
[[nodiscard]] std::vector<std::string>
declared_union(const InstalledPackage& package,
               std::vector<std::string> gc::EventSpec::*member)
{
    std::vector<std::string> names;
    for (const gc::Contribution& contribution : package.contributions)
    {
        for (const std::string& name : contribution.events.*member)
        {
            if (!contains_name(names, name))
            {
                names.push_back(name);
            }
        }
    }
    return names;
}

} // namespace

PackageFactHost::PackageFactHost(PackageStoreScan& scan, PackageGrantHost& grants,
                                 PackageSessionHost& sessions)
    : scan_(scan), grants_(grants), sessions_(sessions)
{
}

const InstalledPackage* PackageFactHost::find_package(const std::string& package_id) const
{
    for (const InstalledPackage& package : scan_.packages)
    {
        if (package.id == package_id)
        {
            return &package;
        }
    }
    return nullptr;
}

std::vector<std::string> PackageFactHost::declared_publishes(const std::string& package_id) const
{
    const InstalledPackage* package = find_package(package_id);
    return package == nullptr ? std::vector<std::string>{}
                              : declared_union(*package, &gc::EventSpec::publishes);
}

std::vector<std::string> PackageFactHost::declared_subscribes(const std::string& package_id) const
{
    const InstalledPackage* package = find_package(package_id);
    return package == nullptr ? std::vector<std::string>{}
                              : declared_union(*package, &gc::EventSpec::subscribes);
}

bool PackageFactHost::may_subscribe(const std::string& package_id, const std::string& topic) const
{
    // An UNINSTALLED package receives nothing: a grant left in the document for a package that is
    // gone stays unusable WHILE IT IS GONE — the same posture `attach_scope_spec_for` takes, and for
    // the same reason (the manifest that would clamp it is not there to clamp with).
    //
    // ⚠ THIS GUARD IS BEHAVIOURALLY REDUNDANT, AND SAYING SO IS THE POINT (the discipline
    // `parse_capability_list` states about its own unknown-token branch): both `declared_*` calls
    // below already answer EMPTY for an unknown package, so deleting this returns `false` by the
    // same route and no assertion in the suite could tell. What it uniquely contributes is one
    // NAMED place where "uninstalled ⇒ nothing" is stated — the rule a reader would otherwise have
    // to reconstruct from two accessors' null handling — plus an exit before two vectors are built.
    // Defence in depth, not the control.
    if (find_package(package_id) == nullptr)
    {
        return false;
    }
    // DECISION 3 — its OWN declared topic, with no grant and no prompt.
    if (contains_name(declared_publishes(package_id), topic))
    {
        return true;
    }
    // DECISION 2 — another package's topic. BOTH clamps, in the order that makes the cheaper,
    // package-authored one answer first: the manifest must have declared an interest, AND the
    // operator must have consented. `granted()` is FALSE for every unknown package and unknown
    // capability, so the grant half is deny-by-default by construction.
    if (!contains_name(declared_subscribes(package_id), topic))
    {
        return false;
    }
    return grants_.grants().granted(package_id, gc::kCapabilityPackageEvents);
}

BridgeResult PackageFactHost::publish(const std::string& package_id, const std::string& topic,
                                      const contract::Json& payload)
{
    // The SAME id predicate the session table and the asset scheme validate against — a package that
    // is one thing to the scheme and another here would publish under an identity nothing mounts.
    if (!is_valid_package_id(package_id))
    {
        return BridgeResult::error(kErrFactsBadParams,
                                   "panel.facts.publish requires a valid 'packageId'");
    }
    if (topic.empty())
    {
        return BridgeResult::error(kErrFactsBadParams,
                                   "panel.facts.publish requires a non-empty string 'topic'");
    }
    // DECISION 4 — THE DECLARATION CHECK, against the SCAN and never against the request.
    //
    // ⚠ ORDERED SO THE MOST SPECIFIC DIAGNOSTIC WINS. A mis-namespaced topic is ALSO an undeclared
    // one, so checking declaration first would report every namespacing mistake as "your manifest
    // does not declare that" and send an author to add an entry the registry would then refuse. The
    // namespacing message names the actual defect.
    if (!is_namespaced_under(topic, package_id))
    {
        ++refused_publishes_;
        return BridgeResult::error(kErrFactsTopicNotNamespaced,
                                   "package '" + package_id + "' may not publish '" + topic +
                                       "': a package fact topic must be namespaced under its own "
                                       "package id ('" +
                                       package_id + ".<name>')");
    }
    if (!contains_name(declared_publishes(package_id), topic))
    {
        ++refused_publishes_;
        return BridgeResult::error(kErrFactsTopicNotDeclared,
                                   "package '" + package_id + "' did not declare '" + topic +
                                       "' in its manifest's events.publishes[]; a package may only "
                                       "publish topics it declared");
    }
    BridgeResult result = sessions_.publish_fact(package_id, topic, payload);
    if (result.error_code.empty())
    {
        ++accepted_publishes_;
    }
    return result;
}

bool PackageFactHost::install(BridgeRouter& router)
{
    // CONTROL 6's POLICY, installed with the route (see the header for why not in the constructor).
    // Both closures capture `this`, whose lifetime is the composition root's — the same lifetime the
    // router and the session host have, so there is no shape in which one outlives another.
    sessions_.set_fact_policy(
        [this](const std::string& package_id) { return declared_publishes(package_id); },
        [this](const std::string& package_id, const std::string& topic)
        { return may_subscribe(package_id, topic); });

    return router.register_method(
        kPanelFactsPublishMethod,
        [this](const BridgeRequest& request) -> BridgeResult
        {
            std::string package_id;
            std::string topic;
            if (!read_string(request.params, "packageId", package_id) ||
                !read_string(request.params, "topic", topic))
            {
                return BridgeResult::error(
                    kErrFactsBadParams,
                    "panel.facts.publish requires string 'packageId' and 'topic'");
            }
            // A MISSING `payload` is REFUSED rather than defaulted to null, and this is the one
            // place that differs from `panel.daemon.call`'s treatment of an absent `params`. There,
            // "no arguments" is a real call. Here, a fact with no value is not a fact — and
            // defaulting it to null would RETAIN null, which then deduplicates against the next
            // deliberate null and makes an author's first real publish silently vanish.
            if (!request.params.is_object() || !request.params.contains("payload"))
            {
                return BridgeResult::error(kErrFactsBadParams,
                                           "panel.facts.publish requires a 'payload'");
            }
            return publish(package_id, topic, request.params.at("payload"));
        });
}

} // namespace context::editor::shell
