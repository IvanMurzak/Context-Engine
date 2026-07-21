// The live diagnostics feed implementation (see problems_feed.h for the design and the tolerance
// rationale).

#include "context/editor/shell/panels/problems_feed.h"

#include "context/editor/shell/panels/builtin_panels.h" // kDiagnosticsTopic / kDerivationTopic

#include <utility>

namespace context::editor::shell::panels
{

namespace
{

// Read an optional string member; returns the empty string when absent or not a string.
[[nodiscard]] std::string read_string(const contract::Json& object, const std::string& key)
{
    if (!object.is_object() || !object.contains(key))
    {
        return std::string();
    }
    const contract::Json& value = object.at(key);
    return value.is_string() ? value.as_string() : std::string();
}

// Read an optional non-negative integer member. Returns 0 when absent, not a number, or negative —
// which is exactly what NavTarget means by "unspecified", so a hostile negative line number
// degrades to "no line" instead of wrapping into a huge unsigned.
[[nodiscard]] std::uint32_t read_u32(const contract::Json& object, const std::string& key)
{
    if (!object.is_object() || !object.contains(key))
    {
        return 0;
    }
    const contract::Json& value = object.at(key);
    if (!value.is_number())
    {
        return 0;
    }
    // Range-check on the DOUBLE, before any integral cast. `as_int()` is a `static_cast<int64_t>`
    // of the stored double, and casting a double outside int64's range is UNDEFINED BEHAVIOUR —
    // which the blocking `sanitize (ASan+UBSan, ubuntu)` leg reports as `float-cast-overflow`.
    // The wire is untrusted (`Json::parse` accepts `1e300` happily), so guarding after the cast
    // would be guarding after the UB had already happened.
    const double raw = value.as_number();
    if (!(raw >= 1.0 && raw <= 4294967295.0))
    {
        return 0;
    }
    return static_cast<std::uint32_t>(raw);
}

// Read a `generation` stamp, falling back to the caller's when absent, unreadable, or out of range.
//
// Range-checked on the DOUBLE for the same reason as `read_u32`: `as_int()` casts the stored double
// and an out-of-range cast is UB (UBSan `float-cast-overflow`). The NEGATIVE case matters
// independently of the UB — a `-1` cast straight to `std::uint64_t` becomes ~1.8e19, and a
// diagnostic stamped with that could never satisfy `generation < settled_generation`, leaving a
// stale provisional row that `on_derivation_settled` can never discard.
[[nodiscard]] std::uint64_t read_generation(const contract::Json& object, std::uint64_t fallback)
{
    if (!object.is_object() || !object.contains("generation"))
    {
        return fallback;
    }
    const contract::Json& value = object.at("generation");
    if (!value.is_number())
    {
        return fallback;
    }
    const double raw = value.as_number();
    if (!(raw >= 0.0 && raw <= 9007199254740992.0))
    {
        return fallback;
    }
    return static_cast<std::uint64_t>(raw);
}

// The location members, wherever the publisher put them. Checked in order: a nested `nav`, then a
// nested `location`, then the payload itself (the flat shape). The FIRST container carrying a `file`
// wins; when none does, the diagnostic is simply not navigable, which the panel already models.
[[nodiscard]] problems::NavTarget read_nav(const contract::Json& payload)
{
    const contract::Json* source = &payload;
    for (const char* key : {"nav", "location"})
    {
        if (payload.is_object() && payload.contains(key) && payload.at(key).is_object() &&
            !read_string(payload.at(key), "file").empty())
        {
            source = &payload.at(key);
            break;
        }
    }

    problems::NavTarget nav;
    nav.file = read_string(*source, "file");
    if (nav.file.empty())
    {
        // A second spelling the CLI surface uses for the same fact. Only consulted when `file` is
        // absent, so a publisher setting both cannot be read inconsistently.
        nav.file = read_string(*source, "path");
    }
    nav.pointer = read_string(*source, "pointer");
    nav.line = read_u32(*source, "line");
    nav.column = read_u32(*source, "column");
    return nav;
}

} // namespace

// --------------------------------------------------------------------------------- pure parsers

problems::Severity parse_severity(const std::string& token)
{
    if (token == "warning" || token == "warn")
    {
        return problems::Severity::warning;
    }
    if (token == "info" || token == "information")
    {
        return problems::Severity::info;
    }
    if (token == "hint")
    {
        return problems::Severity::hint;
    }
    // "error", the empty string, and anything unrecognized. See the header on why unknown escalates
    // rather than degrades.
    return problems::Severity::error;
}

bridge::Stability parse_stability(const std::string& token)
{
    if (token == "unstable")
    {
        return bridge::Stability::unstable;
    }
    if (token == "settling")
    {
        return bridge::Stability::settling;
    }
    return bridge::Stability::stable;
}

std::optional<problems::ProblemDiagnostic> parse_diagnostic(const contract::Json& payload,
                                                            std::uint64_t generation)
{
    if (!payload.is_object())
    {
        return std::nullopt;
    }
    problems::ProblemDiagnostic diagnostic;
    diagnostic.code = read_string(payload, "code");
    diagnostic.message = read_string(payload, "message");
    if (diagnostic.code.empty() && diagnostic.message.empty())
    {
        // Nothing a human could act on and nothing the panel could label a row with. The ONE
        // rejected shape (header: "Tolerance is a design choice").
        return std::nullopt;
    }
    // `key` is the publisher's own stable identity when it supplies one; without it the panel
    // derives a composite identity from code + nav + message, which is what makes the
    // provisional->stable promotion collapse a re-emission instead of duplicating it.
    // DELIBERATELY no `opId` fallback. `opId` names an OPERATION, not a diagnostic: a single
    // crashed op publishes one diagnostic PER PLANNED WRITE (`filesync/src/intent_log.cpp` pushes
    // `filesync.intent.jail` / `.resume` / `.cas` inside the per-write loop, every one carrying the
    // same `entry->op_id`). Adopting it as the identity would make all of them collapse onto one
    // last-wins row — `ProblemDiagnostic::identity()` returns `key` verbatim when non-empty, and
    // both `ingest` and the model's dedup key off that. The composite fallback identity
    // (code + file + pointer + line + column + message) keeps those rows DISTINCT while still
    // collapsing a genuine re-emission, which is exactly what R-FILE-004's "reported, never
    // silently dropped" requires at the panel.
    diagnostic.key = read_string(payload, "key");
    diagnostic.severity = parse_severity(read_string(payload, "severity"));
    diagnostic.stability = parse_stability(read_string(payload, "stability"));
    diagnostic.nav = read_nav(payload);
    diagnostic.generation = generation;
    return diagnostic;
}

std::optional<std::vector<problems::ProblemDiagnostic>>
parse_diagnostics_snapshot(const contract::Json& snapshot, std::uint64_t generation)
{
    std::vector<problems::ProblemDiagnostic> out;

    // Shape 1: a bare array of diagnostic payloads.
    const contract::Json* items = nullptr;
    bool envelopes = false;
    if (snapshot.is_array())
    {
        items = &snapshot;
    }
    else if (snapshot.is_object() && snapshot.contains("diagnostics") &&
             snapshot.at("diagnostics").is_array())
    {
        // Shape 2: {"diagnostics": [...]}.
        items = &snapshot.at("diagnostics");
    }
    else if (snapshot.is_object() && snapshot.contains("events") && snapshot.at("events").is_array())
    {
        // Shape 3: {"events": [{topic, payload, generation}, ...]} — full envelopes, so each item's
        // OWN generation is authoritative and its topic must be filtered.
        items = &snapshot.at("events");
        envelopes = true;
    }
    if (items == nullptr)
    {
        // NO RECOGNIZED CONTAINER = NO INFORMATION, which is NOT the same as "no diagnostics".
        // The subscription snapshot the daemon actually sends is a CURSOR
        // (`EventStream::snapshot()` -> `{incarnationId, generation, lastSeq}`), carrying no
        // diagnostic container at all. Returning an empty SET here would make the caller clear
        // the panel — wiping every diagnostic precisely at the resnapshot that follows a wire gap
        // or a daemon restart, i.e. at the recovery point the panel exists to survive. nullopt
        // says "this snapshot told us nothing about the diagnostic set"; an ENGAGED but empty
        // vector still means "the set is genuinely empty" and still clears.
        return std::nullopt;
    }

    for (std::size_t i = 0; i < items->size(); ++i)
    {
        const contract::Json& item = items->at(i);
        if (!envelopes)
        {
            if (std::optional<problems::ProblemDiagnostic> parsed =
                    parse_diagnostic(item, generation))
            {
                out.push_back(std::move(*parsed));
            }
            continue;
        }
        if (!item.is_object() || read_string(item, "topic") != kDiagnosticsTopic)
        {
            continue;
        }
        const std::uint64_t stamp = read_generation(item, generation);
        if (std::optional<problems::ProblemDiagnostic> parsed =
                parse_diagnostic(item.at("payload"), stamp))
        {
            out.push_back(std::move(*parsed));
        }
    }
    return out;
}

// ------------------------------------------------------------- the node-id -> diagnostic mapping

std::optional<std::string> problems_row_identity(const problems::ProblemsPanel& panel,
                                                 const std::string& node_id)
{
    const std::string prefix = kProblemsRowPrefix;
    if (node_id.size() <= prefix.size() || node_id.compare(0, prefix.size(), prefix) != 0)
    {
        return std::nullopt;
    }

    // Parse the index by hand rather than with std::stoul: the suffix is renderer-controlled, and
    // stoul THROWS on garbage and on overflow. A parse that cannot throw keeps this path total,
    // which is the standing rule for anything reachable from the bridge.
    std::size_t wanted = 0;
    for (std::size_t i = prefix.size(); i < node_id.size(); ++i)
    {
        const char c = node_id[i];
        if (c < '0' || c > '9')
        {
            return std::nullopt;
        }
        if (wanted > (static_cast<std::size_t>(-1) - 9) / 10)
        {
            return std::nullopt; // an index no row could have; refuse rather than wrap
        }
        wanted = wanted * 10 + static_cast<std::size_t>(c - '0');
    }

    // The SAME grouped/row traversal build_panel walks, so index N here is the row build_panel
    // labelled `problems.row.N`.
    const problems::ProblemsModel& model = panel.model(); // cached — a click costs no rebuild
    std::size_t index = 0;
    for (const problems::ProblemGroup& group : model.groups)
    {
        for (const problems::ProblemDiagnostic& diagnostic : group.diagnostics)
        {
            if (index == wanted)
            {
                // A non-navigable row binds no command and so should never reach here; refusing it
                // explicitly means a stale mounted DOM cannot navigate to a diagnostic that has
                // since lost its source location.
                return diagnostic.nav.navigable() ? std::optional<std::string>(diagnostic.identity())
                                                  : std::nullopt;
            }
            ++index;
        }
    }
    return std::nullopt;
}

// ------------------------------------------------------------------------------------ the feed

ProblemsFeed::ProblemsFeed(PanelHost& host, std::string panel_id)
    : host_(host), panel_id_(std::move(panel_id))
{
}

void ProblemsFeed::apply_snapshot(const contract::Json& snapshot, std::uint64_t generation)
{
    // The snapshot's OWN generation wins when it carries one. The real subscription snapshot is a
    // cursor that always does (`EventStream::snapshot()` sets it), and stamping the diagnostics it
    // recovers with a caller-guessed 0 would mark every one of them stale-by-construction: the
    // stream never settles below generation 1, so the first `derivation.settled` would discard
    // every provisional row the snapshot had just restored.
    const std::uint64_t stamp = read_generation(snapshot, generation);
    if (std::optional<std::vector<problems::ProblemDiagnostic>> parsed =
            parse_diagnostics_snapshot(snapshot, stamp))
    {
        panel_.set_diagnostics(std::move(*parsed));
        // Touched only when the snapshot actually SPOKE about the diagnostic set — including when
        // it spoke to say the set is empty: "the diagnostics all cleared" is a change the renderer
        // must see, and a revision that only moved on non-empty snapshots would leave a stale list
        // mounted after the last problem was fixed. A snapshot carrying no diagnostic container
        // (the cursor shape) changes nothing, so it moves no revision either.
        host_.touch(panel_id_);
    }
    ++snapshots_applied_;
}

bool ProblemsFeed::apply_event(const std::string& topic, const contract::Json& payload,
                              std::uint64_t generation)
{
    if (topic == kDiagnosticsTopic)
    {
        std::optional<problems::ProblemDiagnostic> parsed = parse_diagnostic(payload, generation);
        if (!parsed.has_value())
        {
            return false;
        }
        // `ingest` is the promotion path: a re-emitted diagnostic whose stability advanced updates
        // IN PLACE rather than duplicating (problems_panel.h).
        (void)panel_.ingest(std::move(*parsed));
        ++events_applied_;
        host_.touch(panel_id_);
        return true;
    }

    if (topic == kDerivationTopic && read_string(payload, "event") == kDerivationSettledEvent)
    {
        // R-BRIDGE-008: discard stale provisional markers stamped OLDER than the settled generation,
        // then promote the ones stamped WITH it. The world's reported stability rides along for the
        // panel's status line; absent, it is `stable`, which is what "settled" means by default.
        panel_.on_derivation_settled(generation, parse_stability(read_string(payload, "stability")));
        ++events_applied_;
        host_.touch(panel_id_);
        return true;
    }

    return false;
}

PanelProvider ProblemsFeed::make_provider()
{
    PanelProvider provider;
    provider.build = [this] { return panel_.build_panel(); };
    provider.invoke = [this](const std::string& command_id, const contract::Json& params)
    {
        if (command_id != problems::kNavigateCommand)
        {
            return false;
        }
        // The hydration runtime sends the ACTIVATED NODE's id — it knows nothing about diagnostics.
        // `problems_row_identity` is the translation that keeps it that way.
        const std::optional<std::string> identity =
            problems_row_identity(panel_, read_string(params, "nodeId"));
        return identity.has_value() && panel_.navigate(*identity);
    };
    // No `gesture`, no state pair: see the header. Problems is a read-only observer.
    return provider;
}

} // namespace context::editor::shell::panels
