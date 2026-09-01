// T1 for the PACKAGE FACT BUS (editor-UX d2, D4 + D5; design 02 §C / 05 §3 / 08 §2).
//
// ⚠ EVERY "X DID NOT HAPPEN" CLAIM IN THIS FILE HAS A SIBLING PROVING X IS PRODUCIBLE, IN THE SAME
// FIXTURE FAMILY. That is the set's named gate, and it is not a style rule: a short-circuit
// satisfies an absence claim without the mechanism ever running, so "an unconsented subscriber
// received nothing" passes just as happily with the WHOLE BUS DELETED. Each pair below is written so
// that the positive half fails the moment the bus stops working and the negative half fails the
// moment the control stops controlling. The pairs, named so a reviewer can check the list rather
// than trust the prose:
//
//   dedup            same value twice delivers ONCE   <->  a DIFFERENT value delivers twice
//   snapshot         a retained value IS in snapshot  <->  a declared-but-unpublished topic is NOT
//   reentrancy       publish inside a handler REFUSED <->  the same publish outside it SUCCEEDS
//   publish auth     undeclared / mis-namespaced      <->  declared + namespaced PUBLISHES
//   the grant        ungranted subscribe REFUSED      <->  a CONSENTED one is admitted AND delivers
//   the filter       a foreign fact is DROPPED        <->  a granted one arrives on the same pump
//
// THE DAEMON HALF IS DRIVEN THROUGH THE REAL `Dispatcher` + `EventStream`, never a stand-in: D5's
// dedup, retention and reentrancy are properties of the shipping bus or they are properties of
// nothing. The SHELL half is driven through the real `PackageFactHost` + `PackageSessionHost` over
// `MockChannel` wires, which is what lets a test read the exact request that reached the daemon
// rather than trusting that one did.

#include "context/editor/shell/package_facts.h"

#include "context/editor/bridge/dispatcher.h"
#include "context/editor/bridge/event_stream.h"
#include "context/editor/bridge/scope.h"
#include "context/editor/contract/handshake.h"
#include "context/editor/gui/contract/extension.h"
#include "context/editor/contract/registry.h"
#include "context/editor/gui/contract/registry.h"
#include "context/editor/shell/ipc_bridge.h"

#include "mock_channel.h"
#include "shell_test.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace
{
namespace bridge = context::editor::bridge;
namespace client = context::editor::client;
namespace contract = context::editor::contract;
namespace gc = context::editor::gui::contract;
namespace shell = context::editor::shell;

using contract::Json;
using shell::BridgeResult;
using shell::BridgeRouter;
using shell::PackageFactHost;
using shell::PackageGrantHost;
using shell::PackageSessionHost;

// ------------------------------------------------------------------------------------- helpers

[[nodiscard]] Json fact(const std::string& value)
{
    Json out = Json::object();
    out.set("brush", Json(value));
    return out;
}

// A session attached at the PACKAGE BASELINE, through the real parse + attach chain — so every
// daemon-half assertion below runs at exactly the authority a package panel really holds. If the
// bus ever needed more than the baseline, these tests would go red rather than the discovery
// arriving as a support ticket.
[[nodiscard]] const bridge::Session* baseline(const bridge::Dispatcher& dispatcher,
                                              bridge::Dispatcher::AttachResult& storage)
{
    contract::ClientHandshake handshake;
    handshake.protocol_major = contract::kProtocolMajor;
    storage = dispatcher.attach(handshake, bridge::ScopeSet::parse(shell::kPackageSessionScope));
    return std::get_if<bridge::Session>(&storage);
}

[[nodiscard]] Json declare_params(const std::vector<std::string>& topics)
{
    Json list = Json::array();
    for (const std::string& topic : topics)
    {
        list.push_back(Json(topic));
    }
    Json params = Json::object();
    params.set("topics", std::move(list));
    return params;
}

[[nodiscard]] Json publish_params(const std::string& topic, Json payload)
{
    Json params = Json::object();
    params.set("topic", Json(topic));
    params.set("payload", std::move(payload));
    return params;
}

[[nodiscard]] std::string error_code_of(const contract::Envelope& envelope)
{
    return envelope.error().has_value() ? envelope.error()->code : std::string();
}

// ============================ 1. THE DAEMON HALF — D5, on the real bus ============================

// PAIR 1 — DEDUP. The whole cycle breaker: without the negative half this passes against a dead
// topic, and without the positive half it passes against a bus that publishes nothing at all.
void a_repeat_publishes_once_and_a_different_value_publishes_twice()
{
    bridge::EventStream stream("inc-dedup");
    bridge::Dispatcher dispatcher(&stream);
    bridge::Dispatcher::AttachResult attached;
    const bridge::Session* session = baseline(dispatcher, attached);
    CHECK(session != nullptr);
    if (session == nullptr)
    {
        return;
    }
    CHECK(dispatcher.dispatch("events.declare", declare_params({"acme.tilemap.brush"}), *session)
              .ok());

    // A subscriber on the topic — the observable, because "was it delivered" is the only question a
    // consuming package can ask. Counting seqs alone would pass against a bus that emitted without
    // fanning out.
    bridge::Subscriber sub({"acme.tilemap.brush"});
    stream.add_subscriber(&sub);

    const contract::Envelope first =
        dispatcher.dispatch("events.publish", publish_params("acme.tilemap.brush", fact("pencil")),
                            *session);
    CHECK(first.ok());
    CHECK(first.ok() && first.data().at("changed").as_bool());

    // THE REPEAT. Accepted (it is not an error to re-assert a state) and emits NOTHING.
    const contract::Envelope repeat =
        dispatcher.dispatch("events.publish", publish_params("acme.tilemap.brush", fact("pencil")),
                            *session);
    CHECK(repeat.ok());
    CHECK(repeat.ok() && !repeat.data().at("changed").as_bool());
    // `seq` is 0 on a dedup — reporting the previous one would invent an event nobody emitted.
    CHECK(repeat.ok() && repeat.data().at("seq").as_int() == 0);
    CHECK(stream.package_facts_deduplicated() == 1);
    CHECK(sub.drain().size() == 1);

    // THE SIBLING that makes the assertion above mean something: a DIFFERENT value on the SAME topic
    // in the SAME fixture delivers. Delete the retention and this still passes; delete the DELIVERY
    // and only this one goes red — which is exactly why both are here.
    const contract::Envelope changed =
        dispatcher.dispatch("events.publish", publish_params("acme.tilemap.brush", fact("eraser")),
                            *session);
    CHECK(changed.ok());
    CHECK(changed.ok() && changed.data().at("changed").as_bool());
    CHECK(sub.drain().size() == 1);
    stream.remove_subscriber(&sub);
}

// Member ORDER is not a value change. Without canonicalization a producer that builds its payload
// from an unordered container re-publishes on every hop and the mirroring loop D5 exists to break is
// back — with the dedup code fully present and apparently working.
void a_reordered_payload_is_the_same_state_and_deduplicates()
{
    bridge::EventStream stream("inc-canon");
    std::string error;
    CHECK(stream.declare_package_topic("acme.tilemap.brush", error));

    Json first = Json::object();
    first.set("a", Json(1));
    first.set("b", Json(2));
    Json reordered = Json::object();
    reordered.set("b", Json(2));
    reordered.set("a", Json(1));

    CHECK(stream.publish_package_fact("acme.tilemap.brush", first).changed);
    CHECK(!stream.publish_package_fact("acme.tilemap.brush", reordered).changed);
    // THE SIBLING: a genuinely different VALUE in the same shape still gets through, so the test
    // above is not passing because the comparison collapsed everything.
    Json different = Json::object();
    different.set("b", Json(3));
    different.set("a", Json(1));
    CHECK(stream.publish_package_fact("acme.tilemap.brush", different).changed);
}

// PAIR 2 — SNAPSHOT-ON-SUBSCRIBE. The negative half ("a topic with no publish yet delivers
// nothing") is the one that could pass vacuously, so it is asserted IN THE SAME snapshot as a topic
// that does carry a value: if the whole `packageFacts` member were missing, the positive half fails.
void a_late_subscriber_reads_the_retained_value_and_an_unpublished_topic_is_absent()
{
    bridge::EventStream stream("inc-snap");
    bridge::Dispatcher dispatcher(&stream);
    bridge::Dispatcher::AttachResult attached;
    const bridge::Session* session = baseline(dispatcher, attached);
    CHECK(session != nullptr);
    if (session == nullptr)
    {
        return;
    }
    CHECK(dispatcher
              .dispatch("events.declare",
                        declare_params({"acme.tilemap.brush", "acme.tilemap.quiet"}), *session)
              .ok());
    CHECK(dispatcher
              .dispatch("events.publish", publish_params("acme.tilemap.brush", fact("pencil")),
                        *session)
              .ok());

    // SUBSCRIBING AFTER THE FACT — the late panel. It receives the value in its own reply, which is
    // what removes the subscribe-then-separately-ask race.
    const contract::Envelope subscribed =
        dispatcher.dispatch("subscribe", Json::object(), *session);
    CHECK(subscribed.ok());
    if (!subscribed.ok())
    {
        return;
    }
    const Json& facts = subscribed.data().at("snapshot").at("packageFacts");
    CHECK(facts.is_array());
    CHECK(facts.size() == 1);
    CHECK(facts.size() == 1 && facts.at(0).at("topic").as_string() == "acme.tilemap.brush");
    CHECK(facts.size() == 1 && facts.at(0).at("payload").at("brush").as_string() == "pencil");
    // THE NEGATIVE HALF, in the same snapshot: `acme.tilemap.quiet` is DECLARED and has no value, so
    // it is absent. Asserted as an exact size + name rather than "does not contain", because a
    // snapshot that lost the whole array would satisfy a bare absence check.
    CHECK(stream.is_package_topic_declared("acme.tilemap.quiet"));
    CHECK(stream.retained_package_fact("acme.tilemap.quiet") == nullptr);
    CHECK(stream.retained_package_fact("acme.tilemap.brush") != nullptr);
}

// PAIR 3 — REENTRANCY, with the DIAGNOSTIC asserted rather than merely the absence of an event.
void a_publish_from_inside_a_handler_is_refused_with_its_diagnostic()
{
    bridge::EventStream stream("inc-reentry");
    std::string error;
    CHECK(stream.declare_package_topic("acme.a.state", error));
    CHECK(stream.declare_package_topic("acme.b.state", error));

    // The handler is the in-process consumer the guard is armed around (event_stream.h
    // § PublishListener). It mirrors every `acme.a.state` onto `acme.b.state` — the exact A -> B
    // shape D5 names.
    bridge::EventStream::PackageFactResult from_handler;
    std::size_t handler_runs = 0;
    stream.add_publish_listener(
        [&](const bridge::Event& event)
        {
            if (event.topic != "acme.a.state")
            {
                return;
            }
            ++handler_runs;
            from_handler = stream.publish_package_fact("acme.b.state", event.payload);
        });

    CHECK(stream.publish_package_fact("acme.a.state", fact("one")).changed);
    CHECK(handler_runs == 1); // the handler RAN — without this the refusal below proves nothing
    CHECK(!from_handler.accepted);
    CHECK(from_handler.error_code == std::string(bridge::kErrPackageFactReentrant));
    // THE DIAGNOSTIC ITSELF: it names the topic and says WHY, because D5 rule 3 exists to make the
    // remaining loop shape visible to an author, and a refusal with an empty message does not.
    CHECK(shelltest::mentions(from_handler.message, "acme.b.state"));
    CHECK(shelltest::mentions(from_handler.message, "inside an event handler"));
    CHECK(stream.retained_package_fact("acme.b.state") == nullptr);

    // THE SIBLING: the SAME publish, from OUTSIDE the handler, succeeds. Without it the assertions
    // above would pass against a topic that could never be published on at all — which is the whole
    // vacuity failure this suite's header names.
    const bridge::EventStream::PackageFactResult outside =
        stream.publish_package_fact("acme.b.state", fact("one"));
    CHECK(outside.accepted);
    CHECK(outside.changed);
    CHECK(stream.retained_package_fact("acme.b.state") != nullptr);
    // …and the guard RELEASED: a stream stuck "publishing" would refuse everything forever, which
    // would make the line above the only thing standing between this feature and a dead bus.
    CHECK(!stream.publishing());
}

// PAIR 4 — PUBLISH AUTHORIZATION at the DAEMON layer: the grammar and the registry.
void an_undeclared_or_contract_owned_topic_is_refused_and_a_declared_one_publishes()
{
    bridge::EventStream stream("inc-auth");
    bridge::Dispatcher dispatcher(&stream);
    bridge::Dispatcher::AttachResult attached;
    const bridge::Session* session = baseline(dispatcher, attached);
    CHECK(session != nullptr);
    if (session == nullptr)
    {
        return;
    }

    // NOBODY DECLARED IT — deny-by-default: publishing never creates a topic.
    const contract::Envelope undeclared = dispatcher.dispatch(
        "events.publish", publish_params("acme.tilemap.brush", fact("pencil")), *session);
    CHECK(!undeclared.ok());
    CHECK(error_code_of(undeclared) == std::string(bridge::kErrPackageTopicUndeclared));

    // A CONTRACT-OWNED TOPIC CANNOT BE SPELLED, and this is the control that makes the read/query
    // baseline safe for this verb (scope.cpp says so). `session` is the daemon's own human-state
    // topic; a package forging one would be a capability escalation with no scope involved.
    for (const char* core : {"session", "files", "diagnostics", "derivation", "clients", "log"})
    {
        const contract::Envelope forged =
            dispatcher.dispatch("events.publish", publish_params(core, fact("x")), *session);
        CHECK(!forged.ok());
        CHECK(error_code_of(forged) == std::string(bridge::kErrPackageTopicInvalid));
        // …and it cannot be DECLARED either, so there is no two-step around the refusal above.
        CHECK(!dispatcher.dispatch("events.declare", declare_params({core}), *session).ok());
    }

    // THE SIBLING: declared + namespaced publishes. Without it every refusal above is satisfied by a
    // verb that refuses unconditionally.
    CHECK(dispatcher.dispatch("events.declare", declare_params({"acme.tilemap.brush"}), *session)
              .ok());
    const contract::Envelope declared = dispatcher.dispatch(
        "events.publish", publish_params("acme.tilemap.brush", fact("pencil")), *session);
    CHECK(declared.ok());
    CHECK(declared.ok() && declared.data().at("changed").as_bool());
    CHECK(stream.package_facts_refused() == 7); // 1 undeclared + 6 forged core topics
}

// The bounds, which exist because this surface is reachable from untrusted code. Both halves: the
// oversized fact is refused AND an ordinary one on the same topic still lands, so "refused" is not
// the topic being broken.
void an_oversized_fact_is_refused_and_nothing_is_retained()
{
    bridge::EventStream stream("inc-bounds");
    std::string error;
    CHECK(stream.declare_package_topic("acme.big.state", error));

    Json huge = Json::object();
    huge.set("blob", Json(std::string(bridge::kMaxPackageFactBytes + 64, 'x')));
    const bridge::EventStream::PackageFactResult refused =
        stream.publish_package_fact("acme.big.state", huge);
    CHECK(!refused.accepted);
    CHECK(refused.error_code == std::string(bridge::kErrPackageFactTooLarge));
    CHECK(stream.retained_package_fact("acme.big.state") == nullptr);

    CHECK(stream.publish_package_fact("acme.big.state", fact("small")).changed);

    // The registry's own ceiling, driven PAST rather than up to — a bound asserted at N-1 cannot
    // tell a real cap from an off-by-one that never fires.
    bridge::EventStream full("inc-full");
    for (std::size_t i = 0; i < bridge::kMaxPackageTopics; ++i)
    {
        std::string ignored;
        CHECK(full.declare_package_topic("acme.pkg.t" + std::to_string(i), ignored));
    }
    std::string capacity_error;
    CHECK(!full.declare_package_topic("acme.pkg.overflow", capacity_error));
    CHECK(!capacity_error.empty());
    CHECK(full.package_topics().size() == bridge::kMaxPackageTopics);
}

// R-CLI-013 PARITY: a live daemon's `describe` enumerates the package topics it holds, so the CLI
// and an agent discover a package's vocabulary the same way they discover everything else.
void describe_lists_the_live_package_topics()
{
    bridge::EventStream stream("inc-describe");
    bridge::Dispatcher dispatcher(&stream);
    bridge::Dispatcher::AttachResult attached;
    const bridge::Session* session = baseline(dispatcher, attached);
    CHECK(session != nullptr);
    if (session == nullptr)
    {
        return;
    }

    // BEFORE: the contract's own topics and no package ones — the baseline that makes the delta
    // below attributable to the declaration rather than to `describe` having grown for any reason.
    const contract::Envelope before = dispatcher.dispatch("describe", Json::object(), *session);
    CHECK(before.ok());
    const std::size_t core_topics =
        before.ok() ? before.data().at("contract").at("eventTopics").size() : 0;
    CHECK(core_topics >= 6);

    CHECK(dispatcher
              .dispatch("events.declare", declare_params({"acme.tilemap.brush", "acme.tilemap.tile"}),
                        *session)
              .ok());

    const contract::Envelope after = dispatcher.dispatch("describe", Json::object(), *session);
    CHECK(after.ok());
    if (!after.ok())
    {
        return;
    }
    const Json& topics = after.data().at("contract").at("eventTopics");
    CHECK(topics.size() == core_topics + 2);
    bool found_brush = false;
    for (std::size_t i = 0; i < topics.size(); ++i)
    {
        if (topics.at(i).at("name").as_string() == "acme.tilemap.brush")
        {
            found_brush = true;
            CHECK(topics.at(i).at("payloadSchema").at("packageDefined").as_bool());
        }
    }
    CHECK(found_brush);

    // ⚠ AND THE STATIC ARTIFACT DID NOT MOVE. `client_schema()` projects the REGISTRY's describe, so
    // the committed `context-client-schema.json` (and the TS typings generated from it) must be
    // unaffected by a package installing itself — otherwise every install would read as contract
    // drift to the `webui-client-typings-drift` gate.
    const Json registry_describe = contract::Registry::instance().describe();
    CHECK(registry_describe.at("contract").at("eventTopics").size() == core_topics);
}

// ============================== 2. THE SHELL HALF — D4's authorization ===========================

// Every client the host mints, so a test can read what actually reached the daemon.
struct MintedWires
{
    std::vector<clientmock::MockChannel*> channels;
    std::size_t minted = 0;
};

[[nodiscard]] PackageSessionHost::ClientFactory make_factory(MintedWires& wires)
{
    return [&wires](std::string&) -> std::unique_ptr<client::Client>
    {
        auto channel = std::make_unique<clientmock::MockChannel>();
        clientmock::MockChannel* raw = channel.get();
        raw->on("attach",
                [](const clientmock::Request&)
                {
                    Json result = Json::object();
                    result.set("protocolMajor",
                               Json(static_cast<std::uint64_t>(contract::kProtocolMajor)));
                    result.set("clientId", Json(static_cast<std::uint64_t>(11)));
                    result.set("capabilities", Json::array());
                    Json scopes = Json::array();
                    scopes.push_back(Json(std::string("read-query")));
                    result.set("scopes", std::move(scopes));
                    return result;
                });
        raw->on("events.declare",
                [](const clientmock::Request& request)
                {
                    Json data = Json::object();
                    data.set("topics", request.params.at("topics"));
                    return clientmock::MockChannel::ok_envelope(std::move(data));
                });
        raw->on("events.publish",
                [](const clientmock::Request& request)
                {
                    Json data = Json::object();
                    data.set("topic", request.params.at("topic"));
                    data.set("changed", Json(true));
                    data.set("seq", Json(static_cast<std::uint64_t>(1)));
                    return clientmock::MockChannel::ok_envelope(std::move(data));
                });
        raw->on("subscribe",
                [](const clientmock::Request&)
                {
                    Json data = Json::object();
                    data.set("subId", Json(std::string("sub-1")));
                    data.set("snapshot", Json::object());
                    return clientmock::MockChannel::ok_envelope(std::move(data));
                });
        wires.channels.push_back(raw);
        ++wires.minted;
        return std::make_unique<client::Client>(std::move(channel));
    };
}

// One contribution declaring what a package publishes / subscribes / asks for.
[[nodiscard]] gc::Contribution contribution(const std::string& package_id,
                                            std::vector<std::string> publishes,
                                            std::vector<std::string> subscribes,
                                            std::vector<std::string> capabilities)
{
    gc::Contribution c;
    c.id = package_id + ".panel";
    c.package_id = package_id;
    c.title = "Panel";
    c.content.type = gc::ContentType::iframe;
    c.content.entry = "context-ext://" + package_id + "/panel.html";
    c.events.publishes = std::move(publishes);
    c.events.subscribes = std::move(subscribes);
    c.capabilities = std::move(capabilities);
    return c;
}

[[nodiscard]] shell::InstalledPackage package(const std::string& id, gc::Contribution c)
{
    shell::InstalledPackage installed;
    installed.id = id;
    installed.version = "1.0.0";
    installed.contributions.push_back(std::move(c));
    return installed;
}

// THE TWO GRAMMARS AGREE, driven through BOTH implementations on the SAME strings.
//
// `EventStream::package_topic_defect` (daemon) and `manifest_defect` (the GUI contract registry) are
// deliberately separate implementations of one rule — the gui contract library is not on the daemon's
// closure, so they CANNOT share a function — and event_stream.cpp's own header note says they are held
// together "by this note and by the shell suite, which drives one topic through both". This is that
// case, and without it the note is an unbacked claim: a topic declarable in a manifest and refused at
// publish (or the reverse) is a package that installs cleanly and can never broadcast, with no build
// error anywhere.
void the_manifest_grammar_and_the_bus_grammar_accept_the_same_topics()
{
    const auto manifest_accepts = [](const std::string& topic)
    {
        gc::Contribution c = contribution("acme-tilemap", {topic}, {}, {});
        return gc::manifest_defect(c).empty();
    };
    const auto bus_accepts = [](const std::string& topic)
    { return bridge::EventStream::package_topic_defect(topic).empty(); };

    // ACCEPTED by both — the positive half, without which every agreement below is satisfied by two
    // implementations that refuse everything.
    for (const char* good : {"acme-tilemap.brush", "acme-tilemap.brush.size", "acme-tilemap.b2"})
    {
        CHECK(manifest_accepts(good));
        CHECK(bus_accepts(good));
    }
    // REFUSED by both, one row per rule the grammar states.
    for (const char* bad : {"Acme.Brush", "acme_tilemap.brush", "acme-tilemap..brush",
                            ".acme-tilemap.brush", "acme-tilemap.brush.", "acme tilemap.brush"})
    {
        CHECK(!manifest_accepts(bad));
        CHECK(!bus_accepts(bad));
    }
    // ⚠ THE ONE PLACE THEY DIFFER, AND IT IS BY DESIGN RATHER THAN BY DRIFT: a single bare segment.
    // The registry refuses it because it is not namespaced under the DECLARING PACKAGE (a rule that
    // needs the package id, which the daemon does not have); the bus refuses it because every
    // contract-owned topic is exactly one segment (a rule that needs the topic vocabulary, which the
    // registry does not have). Two different reasons, the same answer — which is the property that
    // matters, and it is asserted rather than assumed.
    CHECK(!manifest_accepts("session"));
    CHECK(!bus_accepts("session"));
    CHECK(shelltest::mentions(bridge::EventStream::package_topic_defect("session"),
                              "not namespaced under any package"));
}


// The two-package world every Shell-half case below runs in: `acme-tilemap` publishes a brush fact,
// `beta-paint` wants to read it and declares so, and nobody has consented to anything yet.
struct World
{
    shell::PackageStoreScan scan;
    MintedWires wires;
    std::unique_ptr<PackageSessionHost> sessions;
    std::unique_ptr<PackageGrantHost> grants;
    std::unique_ptr<PackageFactHost> facts;
    std::filesystem::path root;

    explicit World(const char* tag)
    {
        root = shelltest::make_temp_project("ce-facts", tag);
        scan.packages.push_back(package(
            "acme-tilemap", contribution("acme-tilemap", {"acme-tilemap.brush"}, {}, {})));
        scan.packages.push_back(package(
            "beta-paint", contribution("beta-paint", {"beta-paint.stroke"},
                                       {"acme-tilemap.brush"}, {gc::kCapabilityPackageEvents})));
        sessions = std::make_unique<PackageSessionHost>(make_factory(wires));
        grants = std::make_unique<PackageGrantHost>(scan, root / "package-grants.json");
        facts = std::make_unique<PackageFactHost>(scan, *grants, *sessions);
        BridgeRouter router;
        CHECK(facts->install(router));
    }

    ~World() { shelltest::cleanup(root); }

    World(const World&) = delete;
    World& operator=(const World&) = delete;
};

// PAIR 4 (Shell layer) — the manifest DECLARATION check, both directions, plus the namespacing one.
void publishing_is_refused_unless_the_manifest_declared_the_topic()
{
    World world("declare");

    // DECLARED + NAMESPACED — the positive half FIRST, so every refusal below is known not to be a
    // verb that simply refuses everything.
    const BridgeResult published =
        world.facts->publish("acme-tilemap", "acme-tilemap.brush", fact("pencil"));
    CHECK(published.error_code.empty());
    CHECK(world.facts->accepted_publishes() == 1);

    // UNDECLARED: namespaced correctly, but the manifest never claimed it.
    const BridgeResult undeclared =
        world.facts->publish("acme-tilemap", "acme-tilemap.secret", fact("x"));
    CHECK(!undeclared.error_code.empty());
    CHECK(undeclared.error_code == std::string(shell::kErrFactsTopicNotDeclared));
    CHECK(shelltest::mentions(undeclared.error_message, "events.publishes"));

    // MIS-NAMESPACED: a package publishing under ANOTHER package's namespace — the impersonation
    // this rule exists to stop. It is refused with the NAMESPACING code, not the declaration one,
    // because the two send an author to different fixes.
    const BridgeResult foreign =
        world.facts->publish("acme-tilemap", "beta-paint.stroke", fact("x"));
    CHECK(!foreign.error_code.empty());
    CHECK(foreign.error_code == std::string(shell::kErrFactsTopicNotNamespaced));

    // The BARE package id is a namespace, not a member of it.
    const BridgeResult bare = world.facts->publish("acme-tilemap", "acme-tilemap", fact("x"));
    CHECK(!bare.error_code.empty());
    CHECK(bare.error_code == std::string(shell::kErrFactsTopicNotNamespaced));

    // An UNINSTALLED package publishes nothing at all, whatever it names.
    const BridgeResult ghost = world.facts->publish("ghost-pkg", "ghost-pkg.thing", fact("x"));
    CHECK(!ghost.error_code.empty());
    CHECK(ghost.error_code == std::string(shell::kErrFactsTopicNotDeclared));

    CHECK(world.facts->refused_publishes() == 4);
    // …and exactly ONE request reached a daemon session: the refusals were decided BEFORE the wire,
    // so a package probing for topics cannot spend a connection doing it.
    CHECK(world.wires.channels.size() == 1);
    CHECK(world.wires.channels[0]->requests_for("events.publish").size() == 1);
}

// The declaration reaches the daemon at SESSION OPEN — D4's "topics registered at install/load".
void a_packages_declared_topics_are_registered_when_its_session_opens()
{
    World world("register");
    CHECK(world.facts->publish("acme-tilemap", "acme-tilemap.brush", fact("pencil"))
              .error_code.empty());
    CHECK(world.wires.channels.size() == 1);
    if (world.wires.channels.empty())
    {
        return;
    }
    const std::vector<clientmock::Request> declares =
        world.wires.channels[0]->requests_for("events.declare");
    CHECK(declares.size() == 1);
    if (declares.empty())
    {
        return;
    }
    CHECK(declares[0].params.at("topics").size() == 1);
    CHECK(declares[0].params.at("topics").at(0).as_string() == "acme-tilemap.brush");
    // The DECLARE ran BEFORE the publish on the same wire — a registration that arrived afterwards
    // would leave the package's first fact refused on every fresh session.
    const std::vector<clientmock::Request> publishes =
        world.wires.channels[0]->requests_for("events.publish");
    CHECK(publishes.size() == 1);
    CHECK(!publishes.empty() && !declares.empty() && declares[0].id < publishes[0].id);
}

// PAIR 5 — THE GRANT, BOTH HALVES. Neither direction alone proves the gate.
void a_cross_package_subscribe_needs_consent_and_a_consented_one_is_admitted()
{
    World world("grant");

    // A package's OWN topic needs no grant and no prompt (decision 3).
    CHECK(world.facts->may_subscribe("beta-paint", "beta-paint.stroke"));
    // A CONTRACT-OWNED topic is not this control's business — it is the ordinary daemon stream.
    CHECK(world.sessions->may_receive_fact("beta-paint", "session"));

    // UNGRANTED: the manifest declared the interest, but nobody consented.
    CHECK(!world.facts->may_subscribe("beta-paint", "acme-tilemap.brush"));
    Json topics = Json::array();
    topics.push_back(Json(std::string("acme-tilemap.brush")));
    Json subscribe = Json::object();
    subscribe.set("topics", std::move(topics));
    const BridgeResult refused = world.sessions->forward("beta-paint", "subscribe", subscribe);
    CHECK(!refused.error_code.empty());
    CHECK(refused.error_code == std::string(shell::kErrPackageTopicNotGranted));
    CHECK(shelltest::mentions(refused.error_message, "acme-tilemap.brush"));
    CHECK(world.sessions->refused_topics() == 1);
    // The refusal ran BEFORE a session was opened, so probing costs no connection slot.
    CHECK(world.sessions->sessions_open() == 0);

    // THE CONSENT — recorded through the SAME `package.grants.decide` machinery a consent surface
    // uses, not by reaching into the store.
    (void)world.grants->decide("beta-paint", {std::string(gc::kCapabilityPackageEvents)});
    CHECK(world.grants->grants().granted("beta-paint", gc::kCapabilityPackageEvents));

    // GRANTED: the identical request is now admitted and reaches the daemon.
    CHECK(world.facts->may_subscribe("beta-paint", "acme-tilemap.brush"));
    Json again = Json::array();
    again.push_back(Json(std::string("acme-tilemap.brush")));
    Json subscribe_again = Json::object();
    subscribe_again.set("topics", std::move(again));
    const BridgeResult admitted =
        world.sessions->forward("beta-paint", "subscribe", subscribe_again);
    CHECK(admitted.error_code.empty());
    CHECK(world.sessions->sessions_open() == 1);
    CHECK(world.sessions->subscriptions_open("beta-paint") == 1);
}

// THE CLAMP — a grant may never exceed what the manifest declared, on BOTH axes.
void a_grant_cannot_exceed_what_the_manifest_declared()
{
    World world("clamp");

    // AXIS 1 — THE CAPABILITY. `acme-tilemap` declares NO capabilities, so consenting to
    // `package_events` for it records nothing at all (package_grants.h decision 3).
    (void)world.grants->decide("acme-tilemap", {std::string(gc::kCapabilityPackageEvents)});
    CHECK(!world.grants->grants().granted("acme-tilemap", gc::kCapabilityPackageEvents));
    CHECK(world.grants->grants().decided("acme-tilemap")); // it WAS answered — a recorded refusal
    CHECK(!world.facts->may_subscribe("acme-tilemap", "beta-paint.stroke"));

    // AXIS 2 — THE TOPIC. `beta-paint` IS granted `package_events`, and that still does not let it
    // subscribe to a topic its manifest never named. This is the half a single-token grant would
    // have lost, which is why both clamps exist.
    (void)world.grants->decide("beta-paint", {std::string(gc::kCapabilityPackageEvents)});
    CHECK(world.grants->grants().granted("beta-paint", gc::kCapabilityPackageEvents));
    CHECK(world.facts->may_subscribe("beta-paint", "acme-tilemap.brush")); // declared -> admitted
    CHECK(!world.facts->may_subscribe("beta-paint", "acme-tilemap.undeclared")); // never declared
}

// PAIR 6 — THE DELIVERY FILTER, which is what makes the grant bind: `subscribe` with NO topics means
// EVERY topic, and that request names nothing to refuse.
void the_pump_drops_unconsented_facts_and_delivers_granted_ones()
{
    World world("filter");
    // Open the session with an UNFILTERED subscription — the shape the topic check cannot see.
    CHECK(world.sessions->forward("beta-paint", "subscribe", Json::object()).error_code.empty());
    CHECK(world.wires.channels.size() == 1);
    if (world.wires.channels.empty())
    {
        return;
    }
    clientmock::MockChannel* wire = world.wires.channels[0];

    const auto push = [&](const std::string& topic, const std::string& value)
    {
        Json event = Json::object();
        event.set("seq", Json(static_cast<std::uint64_t>(1)));
        event.set("topic", Json(topic));
        event.set("payload", fact(value));
        wire->push_event("sub-1", std::move(event));
    };

    // UNCONSENTED: a foreign fact arrives on the wire and is dropped before the buffer.
    push("acme-tilemap.brush", "pencil");
    // A CONTRACT-OWNED fact on the same pump is untouched — the filter is about package topics, and
    // a filter that swallowed the daemon's own stream would break every panel that reads it.
    push("session", "selection");
    CHECK(world.sessions->pump() == 1);
    CHECK(world.sessions->events_filtered() == 1);
    shell::PackageEventDrain drain = world.sessions->poll_events("beta-paint");
    CHECK(drain.events.size() == 1);
    CHECK(drain.events.size() == 1 && drain.events[0].at("topic").as_string() == "session");
    // A DROPPED-BY-POLICY FACT IS NOT A GAP: `gapped` orders a re-snapshot, and a package that was
    // never entitled to a topic has lost nothing it could act on.
    CHECK(!drain.gapped);
    CHECK(drain.dropped == 0);

    // THE SIBLING: consent, and the SAME topic on the SAME pump now arrives. Without this the drop
    // above would pass against a pump that delivered nothing at all.
    (void)world.grants->decide("beta-paint", {std::string(gc::kCapabilityPackageEvents)});
    push("acme-tilemap.brush", "pencil");
    CHECK(world.sessions->pump() == 1);
    CHECK(world.sessions->events_filtered() == 1); // unchanged — nothing was filtered this round
    shell::PackageEventDrain second = world.sessions->poll_events("beta-paint");
    CHECK(second.events.size() == 1);
    CHECK(second.events.size() == 1 &&
          second.events[0].at("topic").as_string() == "acme-tilemap.brush");
    CHECK(second.events.size() == 1 && second.events[0].at("subId").as_string() == "sub-1");
}

// The LOUD pair still travels once the filter is in the path — the e13c-2 property this task must
// not have quietly broken.
void an_overflow_and_a_daemon_gap_are_still_loud_through_the_filter()
{
    World world("loud");
    (void)world.grants->decide("beta-paint", {std::string(gc::kCapabilityPackageEvents)});
    CHECK(world.sessions->forward("beta-paint", "subscribe", Json::object()).error_code.empty());
    CHECK(world.wires.channels.size() == 1);
    if (world.wires.channels.empty())
    {
        return;
    }
    clientmock::MockChannel* wire = world.wires.channels[0];

    // PAST the buffer's capacity, in `kMaxDrainedFramesPerPump`-sized pumps.
    const std::size_t over = shell::kMaxBufferedEventsPerPackage + 8;
    for (std::size_t i = 0; i < over; ++i)
    {
        Json event = Json::object();
        event.set("seq", Json(static_cast<std::uint64_t>(i + 1)));
        event.set("topic", Json(std::string("acme-tilemap.brush")));
        event.set("payload", fact("v" + std::to_string(i)));
        wire->push_event("sub-1", std::move(event));
    }
    while (world.sessions->pump() > 0)
    {
    }
    wire->push_gap();
    (void)world.sessions->pump();

    shell::PackageEventDrain drain = world.sessions->poll_events("beta-paint");
    CHECK(drain.events.size() == shell::kMaxBufferedEventsPerPackage);
    CHECK(drain.dropped == 8);
    CHECK(drain.gapped);
    // DROP-OLDEST: the SURVIVING head is the 9th value, not the 1st — a panel drawing daemon state
    // needs the current truth, not a stale head.
    CHECK(!drain.events.empty() && drain.events[0].at("payload").at("brush").as_string() == "v8");
}

} // namespace

int main()
{
    a_repeat_publishes_once_and_a_different_value_publishes_twice();
    a_reordered_payload_is_the_same_state_and_deduplicates();
    a_late_subscriber_reads_the_retained_value_and_an_unpublished_topic_is_absent();
    a_publish_from_inside_a_handler_is_refused_with_its_diagnostic();
    an_undeclared_or_contract_owned_topic_is_refused_and_a_declared_one_publishes();
    an_oversized_fact_is_refused_and_nothing_is_retained();
    describe_lists_the_live_package_topics();
    the_manifest_grammar_and_the_bus_grammar_accept_the_same_topics();

    publishing_is_refused_unless_the_manifest_declared_the_topic();
    a_packages_declared_topics_are_registered_when_its_session_opens();
    a_cross_package_subscribe_needs_consent_and_a_consented_one_is_admitted();
    a_grant_cannot_exceed_what_the_manifest_declared();
    the_pump_drops_unconsented_facts_and_delivers_granted_ones();
    an_overflow_and_a_daemon_gap_are_still_loud_through_the_filter();
    SHELL_TEST_MAIN_END();
}
