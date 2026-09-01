// Client-facing event stream implementation (see event_stream.h).

#include "context/editor/bridge/event_stream.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace context::editor::bridge
{

using contract::Json;

const char* stability_name(Stability s)
{
    switch (s)
    {
    case Stability::stable:
        return "stable";
    case Stability::unstable:
        return "unstable";
    case Stability::settling:
        return "settling";
    }
    return "stable";
}

namespace
{
const char* log_level_name(kernel::LogLevel level)
{
    switch (level)
    {
    case kernel::LogLevel::trace:
        return "trace";
    case kernel::LogLevel::debug:
        return "debug";
    case kernel::LogLevel::info:
        return "info";
    case kernel::LogLevel::warn:
        return "warn";
    case kernel::LogLevel::error:
        return "error";
    }
    return "info";
}

// A process-unique incarnation id: a steady-clock stamp plus a monotonic counter so two daemons
// started in the same tick still differ. Hex-encoded, "inc-" prefixed, grep-friendly.
std::string generate_incarnation_id()
{
    static std::atomic<std::uint64_t> counter{0};
    const std::uint64_t n = counter.fetch_add(1, std::memory_order_relaxed);
    const auto ticks = static_cast<std::uint64_t>(
        std::chrono::steady_clock::now().time_since_epoch().count());

    static const char* hex = "0123456789abcdef";
    std::string out = "inc-";
    const std::uint64_t mixed = ticks ^ (n * 0x9e3779b97f4a7c15ull);
    for (int shift = 60; shift >= 0; shift -= 4)
        out.push_back(hex[(mixed >> shift) & 0xf]);
    return out;
}

// Is `path` within the `scope` subtree? A prefix match with a component boundary — `scope` itself,
// or a strict child under `scope/…` — so scope "a/b" matches "a/b" and "a/b/c" but never "a/bc".
bool path_within_scope(const std::string& path, const std::string& scope)
{
    if (path == scope)
        return true;
    return path.size() > scope.size() && path.compare(0, scope.size(), scope) == 0 &&
           path[scope.size()] == '/';
}

// --- the package fact bus's helpers (editor-UX d2, D4/D5) ---------------------------------------

// Is `segment` a `[a-z0-9][a-z0-9-]*` name segment? BYTE-FOR-BYTE the grammar
// `gui/contract/src/registry.cpp`'s `is_name_segment` enforces on a manifest's declared names, and
// deliberately so: this bus refuses at publish exactly what the registry refused at registration, so
// a topic can never be declarable in a manifest and unpublishable here (or the reverse). The two
// cannot share a function — the gui contract library is not on the daemon's closure — so they are
// held together by this note and by the shell suite, which drives one topic through both.
bool is_topic_segment(std::string_view segment)
{
    if (segment.empty())
        return false;
    const auto first = static_cast<unsigned char>(segment.front());
    if (!((first >= 'a' && first <= 'z') || (first >= '0' && first <= '9')))
        return false;
    for (const char ch : segment)
    {
        const auto c = static_cast<unsigned char>(ch);
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-'))
            return false;
    }
    return true;
}

// How many dot-separated segments `name` has, or 0 when any of them is malformed. A leading dot, a
// trailing dot and a doubled dot all produce an EMPTY segment, which `is_topic_segment` refuses — so
// the three need no cases of their own (registry.cpp § is_segmented_name states the same).
std::size_t topic_segment_count(const std::string& name)
{
    std::size_t count = 0;
    std::size_t start = 0;
    while (true)
    {
        const std::size_t dot = name.find('.', start);
        const std::string_view segment(name.data() + start,
                                       (dot == std::string::npos ? name.size() : dot) - start);
        if (!is_topic_segment(segment))
            return 0;
        ++count;
        if (dot == std::string::npos)
            return count;
        start = dot + 1;
    }
}

// A CANONICAL serialization of `value`: identical to `dump(0)` except that object members are
// emitted in sorted key order, recursively.
//
// ⚠ THIS IS WHAT MAKES D5's DEDUP — AND THEREFORE THE CYCLE BREAKER — ROBUST. `Json` preserves
// INSERTION order (json.h says so), so `{"a":1,"b":2}` and `{"b":2,"a":1}` dump to different bytes
// while being the same state. Comparing raw dumps would let a package defeat the whole loop
// protection by accident: any producer that builds its payload from an unordered map re-emits the
// same fact with a different member order on every hop, every publish reads as a CHANGE, and the
// A -> B -> A mirror spins exactly as it would with no dedup at all. Sorting here costs one
// serialization per publish and removes that entire failure mode.
//
// It is deliberately NOT the R-FILE-001 canonical serializer: that one lives in `src/editor/serializer`,
// which the bridge does not (and must not) link, and its job is the on-disk byte form of AUTHORED
// data. What this needs is only a stable EQUALITY key for an ephemeral fact, so a local, dependency-free
// projection is the honest tool.
std::string canonical_json(const Json& value)
{
    switch (value.type())
    {
    case Json::Type::array:
    {
        std::string out = "[";
        for (std::size_t i = 0; i < value.size(); ++i)
        {
            if (i != 0)
                out += ',';
            out += canonical_json(value.at(i));
        }
        out += ']';
        return out;
    }
    case Json::Type::object:
    {
        std::vector<const std::pair<std::string, Json>*> members;
        members.reserve(value.object_members().size());
        for (const std::pair<std::string, Json>& member : value.object_members())
            members.push_back(&member);
        std::sort(members.begin(), members.end(),
                  [](const std::pair<std::string, Json>* a, const std::pair<std::string, Json>* b)
                  { return a->first < b->first; });
        std::string out = "{";
        bool first = true;
        for (const std::pair<std::string, Json>* member : members)
        {
            if (!first)
                out += ',';
            first = false;
            // The KEY is emitted through Json's own string dumper so escaping stays one
            // implementation — a hand-rolled quote here would diverge on the first control
            // character and produce two canonical forms of one payload.
            out += Json(member->first).dump(0);
            out += ':';
            out += canonical_json(member->second);
        }
        out += '}';
        return out;
    }
    default:
        return value.dump(0);
    }
}
} // namespace

Json Event::to_json() const
{
    Json out = Json::object();
    out.set("seq", Json(seq));
    out.set("incarnationId", Json(incarnation_id));
    out.set("generation", Json(generation));
    out.set("topic", Json(topic));
    out.set("payload", payload);
    return out;
}

Subscriber::Subscriber(std::vector<std::string> topics, std::size_t capacity, std::string path_scope)
    : topics_(std::move(topics)), path_scope_(std::move(path_scope)),
      capacity_(capacity == 0 ? 1 : capacity)
{
}

bool Subscriber::wants(const std::string& topic) const
{
    if (topics_.empty())
        return true; // no filter => every topic
    for (const std::string& t : topics_)
        if (t == topic)
            return true;
    return false;
}

bool Subscriber::accepts(const Event& e) const
{
    if (!wants(e.topic))
        return false;
    if (path_scope_.empty())
        return true;
    // Path-scoped: a path-bearing event is delivered only when its payload `path` is within the
    // scope subtree. A pathless event (session/clients/log lifecycle) is not a path-scoped fact and
    // always passes — narrowing to a subtree must not hide the daemon's own lifecycle stream.
    if (e.payload.type() != contract::Json::Type::object || !e.payload.contains("path"))
        return true;
    const contract::Json& p = e.payload.at("path");
    if (!p.is_string())
        return true;
    return path_within_scope(p.as_string(), path_scope_);
}

void Subscriber::offer(const Event& e)
{
    if (queue_.size() >= capacity_)
    {
        // Overflow: drop and mark the gap — never block the stream on a slow client.
        gap_ = true;
        return;
    }
    last_seq_ = e.seq;
    queue_.push_back(e);
}

std::vector<Event> Subscriber::drain()
{
    std::vector<Event> out(queue_.begin(), queue_.end());
    queue_.clear();
    return out;
}

EventStream::EventStream() : EventStream(generate_incarnation_id()) {}

EventStream::EventStream(std::string incarnation_id, std::size_t ring_capacity)
    : incarnation_id_(std::move(incarnation_id)), ring_capacity_(ring_capacity == 0 ? 1
                                                                                    : ring_capacity)
{
}

std::uint64_t EventStream::emit(const std::string& topic, Json payload)
{
    Event e;
    e.seq = ++last_seq_;
    e.incarnation_id = incarnation_id_;
    e.generation = generation_;
    e.topic = topic;
    e.payload = std::move(payload);

    ring_.push_back(e);
    prune_ring();

    // ⚠ THE GUARD SPANS THE WHOLE FAN-OUT, AND IT IS SET FOR **EVERY** PUBLISH — a settle, a
    // forwarded kernel log, a session fact, a package fact. D5's reentrancy rule is "a publish
    // issued from inside an event handler is refused", and the only honest reading of "inside a
    // handler" is "while this stream is delivering". Arming it only around `publish_package_fact`
    // would leave the reachable in-process re-entry — a kernel EventBus handler running under
    // `forward_log` — outside the guard, which is exactly the shape that produces a loop nobody can
    // see. RAII rather than a flag pair, so a throwing listener cannot leave the stream permanently
    // "publishing" and refuse every later fact.
    //
    // ⚠ IT RESTORES THE PREVIOUS VALUE RATHER THAN CLEARING. A listener that publishes anything at
    // all (a settle, a forwarded log) nests one `emit` inside another, and a scope that cleared on
    // exit would DISARM the guard for the whole remainder of the outer fan-out — so the very next
    // listener could publish a package fact from inside a handler and be accepted, which is the loop
    // D5 rule 3 exists to refuse. Save-and-restore makes the flag mean "some fan-out is live",
    // whatever the nesting depth.
    struct PublishScope
    {
        bool& flag;
        const bool previous;
        explicit PublishScope(bool& f) : flag(f), previous(f) { flag = true; }
        ~PublishScope() { flag = previous; }
        PublishScope(const PublishScope&) = delete;
        PublishScope& operator=(const PublishScope&) = delete;
    } scope(publishing_);

    for (Subscriber* sub : subscribers_)
        if (sub != nullptr && sub->accepts(e))
            sub->offer(e);

    // A COPY of the listener vector, for the reason every fan-out over a mutable container needs
    // one: a listener may add or drop one, which would invalidate the iterator mid-delivery.
    for (const PublishListener& listener : std::vector<PublishListener>(publish_listeners_))
        if (listener)
            listener(e);

    return e.seq;
}

std::uint64_t EventStream::publish(const std::string& topic, Json payload,
                                   std::optional<Stability> stability)
{
    // Diagnostics ALWAYS carry a stability field (default stable); other topics only when supplied.
    if (topic == "diagnostics" && !stability.has_value())
        stability = Stability::stable;
    if (stability.has_value())
    {
        if (payload.type() != Json::Type::object)
            payload = Json::object();
        payload.set("stability", Json(std::string(stability_name(*stability))));
    }
    return emit(topic, std::move(payload));
}

std::uint64_t EventStream::settle(std::uint64_t derived_generation)
{
    // ADOPT, never increment (see the header). The derived world owns this number; this stream only
    // stamps it, so the envelope's `generation` and the payload's agree by construction rather than
    // by two publishers happening to be called in the right order.
    generation_ = derived_generation;
    Json payload = Json::object();
    payload.set("event", Json(std::string("derivation.settled")));
    payload.set("generation", Json(generation_));
    return emit("derivation", std::move(payload));
}

std::uint64_t EventStream::forward_log(const kernel::LogEvent& e)
{
    Json payload = Json::object();
    payload.set("level", Json(std::string(log_level_name(e.level))));
    payload.set("message", Json(e.message));
    return emit("log", std::move(payload));
}

void EventStream::add_subscriber(Subscriber* sub)
{
    if (sub != nullptr)
        subscribers_.push_back(sub);
}

void EventStream::remove_subscriber(Subscriber* sub)
{
    for (auto it = subscribers_.begin(); it != subscribers_.end(); ++it)
    {
        if (*it == sub)
        {
            subscribers_.erase(it);
            return;
        }
    }
}

void EventStream::add_publish_listener(PublishListener listener)
{
    if (listener)
        publish_listeners_.push_back(std::move(listener));
}

// --- the package fact bus (editor-UX d2, D4/D5) --------------------------------------------------

std::string EventStream::package_topic_defect(const std::string& topic)
{
    if (topic.empty())
        return "a package topic name is empty";
    if (topic.size() > kMaxPackageTopicLength)
        return "package topic \"" + topic.substr(0, kMaxPackageTopicLength) +
               "…\" exceeds " + std::to_string(kMaxPackageTopicLength) + " bytes";
    const std::size_t segments = topic_segment_count(topic);
    if (segments == 0)
        return "\"" + topic + "\" is not a valid topic name (lowercase dotted segments)";
    // THE TWO-SEGMENT FLOOR IS THE CONTRACT-TOPIC DEFENCE, not a style rule — see the header. Every
    // core topic (`files`, `derivation`, `diagnostics`, `session`, `clients`, `log`) is one bare
    // segment, so refusing a single-segment name is what makes it structurally impossible for a
    // package to forge a `session` fact through this bus.
    if (segments < 2)
        return "\"" + topic +
               "\" is not namespaced under any package (a package fact topic is "
               "\"<package-id>.<name>\"; an unnamespaced name is contract-owned)";
    return {};
}

EventStream::PackageTopic* EventStream::find_package_topic(const std::string& topic)
{
    for (PackageTopic& entry : package_topics_)
        if (entry.name == topic)
            return &entry;
    return nullptr;
}

const EventStream::PackageTopic* EventStream::find_package_topic(const std::string& topic) const
{
    for (const PackageTopic& entry : package_topics_)
        if (entry.name == topic)
            return &entry;
    return nullptr;
}

bool EventStream::declare_package_topic(const std::string& topic, std::string& error)
{
    error.clear();
    if (const std::string defect = package_topic_defect(topic); !defect.empty())
    {
        error = defect;
        return false;
    }
    // IDEMPOTENT, AND THE RETAINED VALUE SURVIVES. A re-declare happens on every reconnect of every
    // package that owns the topic; resetting the value there would silently drop the retained state
    // a late subscriber depends on, which is D5 rule 2 undone by the bookkeeping rather than by the
    // delivery.
    if (find_package_topic(topic) != nullptr)
        return true;
    if (package_topics_.size() >= kMaxPackageTopics)
    {
        error = "the package-topic registry is full (" + std::to_string(kMaxPackageTopics) +
                " topics); \"" + topic + "\" was not registered";
        return false;
    }
    PackageTopic entry;
    entry.name = topic;
    package_topics_.push_back(std::move(entry));
    return true;
}

std::vector<std::string> EventStream::package_topics() const
{
    std::vector<std::string> out;
    out.reserve(package_topics_.size());
    for (const PackageTopic& entry : package_topics_)
        out.push_back(entry.name);
    std::sort(out.begin(), out.end());
    return out;
}

bool EventStream::is_package_topic_declared(const std::string& topic) const
{
    return find_package_topic(topic) != nullptr;
}

const Json* EventStream::retained_package_fact(const std::string& topic) const
{
    const PackageTopic* entry = find_package_topic(topic);
    return (entry != nullptr && entry->has_value) ? &entry->value : nullptr;
}

EventStream::PackageFactResult EventStream::publish_package_fact(const std::string& topic,
                                                                 Json payload)
{
    PackageFactResult result;
    // ⚠ D5 RULE 3 IS CHECKED FIRST, BEFORE THE TOPIC IS EVEN LOOKED UP. A reentrant publish must be
    // refused for BEING reentrant, not for whatever else happens to be wrong with it — a package
    // author who gets `package.topic_undeclared` from inside a handler debugs a manifest that is
    // fine, and the loop the guard exists to name stays invisible.
    if (publishing_)
    {
        ++package_facts_refused_;
        result.error_code = kErrPackageFactReentrant;
        result.message = "a package fact may not be published from inside an event handler (the "
                         "publish on \"" +
                         topic +
                         "\" was refused): a fact is a STATE, and a handler that publishes in "
                         "response to one is the loop shape D5 makes diagnosable";
        return result;
    }
    PackageTopic* entry = find_package_topic(topic);
    if (entry == nullptr)
    {
        ++package_facts_refused_;
        // The GRAMMAR defect is reported when there is one, because "malformed" and "nobody
        // declared it" send an author to two different files, and a single code would make one of
        // them read the wrong one.
        const std::string defect = package_topic_defect(topic);
        result.error_code = defect.empty() ? kErrPackageTopicUndeclared : kErrPackageTopicInvalid;
        result.message = defect.empty() ? ("no package declared the topic \"" + topic +
                                           "\" on this daemon (a topic is registered from its "
                                           "declaring package's manifest at load)")
                                        : defect;
        return result;
    }
    std::string canonical = canonical_json(payload);
    if (canonical.size() > kMaxPackageFactBytes)
    {
        ++package_facts_refused_;
        result.error_code = kErrPackageFactTooLarge;
        result.message = "the fact published on \"" + topic + "\" is " +
                         std::to_string(canonical.size()) + " bytes, over the " +
                         std::to_string(kMaxPackageFactBytes) +
                         "-byte per-topic ceiling; nothing was retained or delivered";
        return result;
    }
    // ⚠ D5 RULE 1 — THE CYCLE BREAKER. A repeat is ACCEPTED and emits NOTHING: no seq is consumed,
    // no ring entry is written, no subscriber is offered anything. That is what makes a refusal, a
    // dedup and "the publish never happened" indistinguishable from every subscriber's side, and it
    // is what makes an A -> B -> A mirror converge after ONE round.
    if (entry->has_value && entry->canonical == canonical)
    {
        ++package_facts_deduplicated_;
        result.accepted = true;
        result.changed = false;
        return result;
    }
    entry->has_value = true;
    entry->value = payload;
    entry->canonical = std::move(canonical);
    result.accepted = true;
    result.changed = true;
    result.seq = emit(topic, std::move(payload));
    return result;
}

Json EventStream::snapshot() const
{
    Json out = Json::object();
    out.set("incarnationId", Json(incarnation_id_));
    out.set("generation", Json(generation_));
    out.set("lastSeq", Json(last_seq_));
    // D5 RULE 2 — SNAPSHOT-ON-SUBSCRIBE, WHICH FALLS OUT OF RETENTION RATHER THAN BEING A SECOND
    // MECHANISM. A panel mounted mid-session reads the CURRENT value of every package topic out of
    // its own `subscribe` reply, so "subscribe, then separately ask for current state" — the race
    // the `editor.ui` bus's model exists to remove — does not reappear here.
    //
    // An ARRAY OF OBJECTS CARRYING THEIR KEY, never a map keyed by topic: the convention the camera
    // array and c1's `selections` already follow, and the one that keeps a topic name pure data
    // rather than a JSON member name. Only topics with a value appear — a DECLARED topic nobody has
    // published on is deliberately absent, which is what makes "a topic with no publish yet
    // delivers nothing" an assertion that can fail.
    //
    // ⚠ SORTED, so two daemons holding the same facts answer byte-identically regardless of the
    // order packages happened to register in. The registry is insertion-ordered (declaration order
    // is load order, which is directory order), so an unsorted projection would make this reply
    // depend on the filesystem.
    std::vector<const PackageTopic*> valued;
    for (const PackageTopic& entry : package_topics_)
        if (entry.has_value)
            valued.push_back(&entry);
    std::sort(valued.begin(), valued.end(),
              [](const PackageTopic* a, const PackageTopic* b) { return a->name < b->name; });
    Json facts = Json::array();
    for (const PackageTopic* entry : valued)
    {
        Json fact = Json::object();
        fact.set("topic", Json(entry->name));
        fact.set("payload", entry->value);
        facts.push_back(std::move(fact));
    }
    out.set("packageFacts", std::move(facts));
    return out;
}

std::vector<Event> EventStream::replay_since(std::uint64_t since, bool& gapped) const
{
    gapped = false;
    if (ring_.empty())
        // Slowest-acked retention (R-CLI-015) can drain the ring entirely while last_seq_ > 0 (every
        // retained event acked by all live subscribers). A stale reconnect (since < last_seq_) then
        // predates retention just as it does when the ring is non-empty: gap => fresh snapshot. Only
        // `since == last_seq_` is genuinely caught-up (nothing after `since` existed to evict).
        gapped = since < last_seq_;
    else if (ring_.front().seq > since + 1)
        gapped = true; // events after `since` were already evicted — fresh snapshot needed

    std::vector<Event> out;
    for (const Event& e : ring_)
        if (e.seq > since)
            out.push_back(e);
    return out;
}

std::uint64_t EventStream::slowest_acked_seq() const
{
    // No live subscriptions => floor 0: pure size-cap retention, preserving the reconnect
    // "since seq N" window for a client that has not (re)subscribed yet.
    if (subscriptions_.empty())
        return 0;
    std::uint64_t floor = subscriptions_.front().acked_seq;
    for (const Subscription& s : subscriptions_)
        floor = std::min(floor, s.acked_seq);
    return floor;
}

void EventStream::prune_ring()
{
    const std::uint64_t floor = slowest_acked_seq();
    // Evict the oldest event while EITHER the hard capacity is exceeded (bounded memory — an
    // over-slow subscriber must never grow the ring without bound) OR it has been acked by every
    // live subscriber (seq <= floor), so no live subscriber still needs it for catch-up.
    while (!ring_.empty() && (ring_.size() > ring_capacity_ || ring_.front().seq <= floor))
        ring_.pop_front();
}

EventStream::SubscribeResult EventStream::subscribe(std::vector<std::string> topics,
                                                    std::string path_scope,
                                                    std::optional<std::uint64_t> since_seq,
                                                    std::size_t capacity)
{
    SubscribeResult result;
    result.sub_id = "sub-" + std::to_string(++sub_counter_);

    // Snapshot-then-delta (R-BRIDGE-008): the client reads this current-state snapshot first; live
    // deltas then arrive on the subscription's queue (drained via poll()).
    result.snapshot = snapshot();

    // The initial ack cursor. A fresh subscriber has "seen" state through the snapshot's lastSeq, so
    // it does not pin ring history at or below it. A reconnect with since_seq resumes from there —
    // its cursor sits at since_seq until it acks the replayed catch-up forward, so retention keeps
    // that history pinned for it (bounded by ring_capacity).
    std::uint64_t acked = last_seq_;
    if (since_seq.has_value())
    {
        result.catchup = replay_since(*since_seq, result.gapped);
        if (*since_seq < acked)
            acked = *since_seq;
    }

    auto owned = std::make_unique<Subscriber>(std::move(topics), capacity, std::move(path_scope));
    Subscriber* raw = owned.get();
    subscriptions_.push_back({result.sub_id, std::move(owned), acked});
    add_subscriber(raw);
    return result;
}

bool EventStream::unsubscribe(const std::string& sub_id)
{
    for (auto it = subscriptions_.begin(); it != subscriptions_.end(); ++it)
    {
        if (it->id == sub_id)
        {
            remove_subscriber(it->sub.get());
            subscriptions_.erase(it);
            prune_ring(); // dropping the slowest subscriber may raise the retention floor
            return true;
        }
    }
    return false;
}

bool EventStream::ack(const std::string& sub_id, std::uint64_t seq)
{
    for (Subscription& s : subscriptions_)
    {
        if (s.id == sub_id)
        {
            if (seq > s.acked_seq) // monotonic: a stale/duplicate ack is a no-op, never a rewind
                s.acked_seq = seq;
            prune_ring(); // advancing the slowest cursor ages out newly-acked history
            return true;
        }
    }
    return false;
}

std::vector<Event> EventStream::poll(const std::string& sub_id)
{
    for (Subscription& s : subscriptions_)
        if (s.id == sub_id)
            return s.sub->drain();
    return {};
}

bool EventStream::sub_gapped(const std::string& sub_id) const
{
    for (const Subscription& s : subscriptions_)
        if (s.id == sub_id)
            return s.sub->gap();
    return false;
}

void EventStream::reset_sub_gap(const std::string& sub_id)
{
    for (Subscription& s : subscriptions_)
        if (s.id == sub_id)
        {
            s.sub->reset_gap();
            return;
        }
}

} // namespace context::editor::bridge
