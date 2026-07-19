// Subscription-consumer implementation (see subscription.h).

#include "context/editor/client/subscription.h"

#include <algorithm>
#include <chrono>
#include <thread>
#include <utility>

namespace context::editor::client
{

using contract::Json;

namespace
{
// The subscribe/ack/unsubscribe replies are R-CLI-008 envelopes: {ok, data:{…}}. Reach the data
// object, or a null Json when the envelope is shaped otherwise.
const Json& envelope_data(const Json& result)
{
    static const Json kNull;
    if (result.is_object() && result.contains("data") && result.at("data").is_object())
        return result.at("data");
    return kNull;
}
} // namespace

std::optional<ClientEvent> parse_event(const Json& envelope)
{
    if (!envelope.is_object() || !envelope.contains("seq") || !envelope.at("seq").is_number())
        return std::nullopt;
    ClientEvent event;
    event.seq = static_cast<std::uint64_t>(envelope.at("seq").as_int());
    if (envelope.contains("incarnationId") && envelope.at("incarnationId").is_string())
        event.incarnation_id = envelope.at("incarnationId").as_string();
    if (envelope.contains("generation") && envelope.at("generation").is_number())
        event.generation = static_cast<std::uint64_t>(envelope.at("generation").as_int());
    if (envelope.contains("topic") && envelope.at("topic").is_string())
        event.topic = envelope.at("topic").as_string();
    if (envelope.contains("payload"))
        event.payload = envelope.at("payload");
    return event;
}

int BackoffPolicy::delay_for_attempt(int attempt) const
{
    // A multiplier of 1 or less never grows the delay, so the loop below would spin `attempt` times
    // to return initial_ms: answer the fixed-delay policy directly instead. This also keeps the
    // retry-forever path (max_attempts == 0) from doing O(attempt) work per attempt.
    if (attempt <= 0 || multiplier <= 1)
        return std::min(initial_ms, max_ms);
    long long delay = initial_ms;
    for (int i = 0; i < attempt; ++i)
    {
        delay *= multiplier;
        if (delay >= max_ms)
            return max_ms;
    }
    return static_cast<int>(delay);
}

SubscriptionConsumer::SubscriptionConsumer(Client& client, AttachOptions attach, Options options)
    : client_(client), attach_(std::move(attach)), options_(options)
{
}

std::size_t SubscriptionConsumer::add(SubscriptionSpec spec, std::string* error)
{
    SubscriptionState state;
    state.spec = std::move(spec);
    states_.push_back(std::move(state));
    const std::size_t index = states_.size() - 1;
    if (started_)
    {
        std::string local_error;
        if (!subscribe_one(states_[index], std::nullopt, local_error))
        {
            states_[index].live = false;
            if (error != nullptr)
                *error = std::move(local_error);
        }
    }
    return index;
}

void SubscriptionConsumer::unsubscribe_one(SubscriptionState& state)
{
    if (!state.live || state.sub_id.empty())
        return;
    Json params = Json::object();
    params.set("subId", Json(state.sub_id));
    std::string ignored;
    (void)client_.call("unsubscribe", std::move(params), ignored);
    state.live = false;
    state.sub_id.clear();
}

bool SubscriptionConsumer::subscribe_one(SubscriptionState& state,
                                         std::optional<std::uint64_t> since, std::string& error,
                                         bool* rejected_by_daemon)
{
    // Release the CURRENT subscription before minting its replacement. Without this every gap /
    // incarnation recovery permanently adds a daemon-side subscription on a still-live connection:
    // the daemon then pushes each event once per leaked subId, filling the connection's bounded
    // outbound budget faster, which produces MORE gaps, which leak MORE subscriptions — a
    // self-amplifying loop. (On the reconnect path the caller has already cleared `live`, because
    // the old connection — and with it the daemon's whole session — is gone.)
    unsubscribe_one(state);

    Json params = Json::object();
    Json topics = Json::array();
    for (const std::string& t : state.spec.topics)
        topics.push_back(Json(t));
    params.set("topics", std::move(topics));
    if (!state.spec.path_scope.empty())
        params.set("pathScope", Json(state.spec.path_scope));
    if (since.has_value())
        params.set("sinceSeq", Json(*since));

    const std::optional<Json> result =
        client_.call("subscribe", std::move(params), error, rejected_by_daemon);
    if (!result.has_value())
        return false;

    const Json& data = envelope_data(*result);
    if (!data.contains("subId") || !data.at("subId").is_string())
    {
        // The daemon answered, just malformed. Class it with a refusal, not a transport fault:
        // reconnecting cannot repair a reply the peer builds wrong.
        error = "subscribe reply carried no subId";
        if (rejected_by_daemon != nullptr)
            *rejected_by_daemon = true;
        return false;
    }
    state.sub_id = data.at("subId").as_string();
    state.live = true;

    // A resume the daemon could not honor (its retained history no longer covers our cursor) reports
    // gapped — the snapshot it returned IS the recovery, so fall through to the snapshot path.
    const bool gapped = data.contains("gapped") && data.at("gapped").as_bool();
    const bool resumed = since.has_value() && !gapped;

    if (data.contains("snapshot"))
    {
        state.snapshot = data.at("snapshot");
        if (state.snapshot.is_object() && state.snapshot.contains("incarnationId") &&
            state.snapshot.at("incarnationId").is_string())
            note_incarnation(state.snapshot.at("incarnationId").as_string());
    }

    if (resumed)
    {
        // Snapshot-then-delta on a resume: apply the replayed catch-up in order, advancing the cursor.
        if (data.contains("catchup") && data.at("catchup").is_array())
        {
            const Json& catchup = data.at("catchup");
            for (std::size_t i = 0; i < catchup.size(); ++i)
                if (const std::optional<ClientEvent> event = parse_event(catchup.at(i)))
                    apply_event(state.sub_id, *event);
        }
    }
    else
    {
        // A FRESH snapshot resets the cursor: seqs before it are represented by the snapshot itself,
        // so resuming from an older cursor would double-apply them.
        ++stats_.snapshots_taken;
        if (gapped)
            ++stats_.gaps_recovered;
        state.last_seq = 0;
        state.acked_seq = 0;
        if (state.snapshot.is_object() && state.snapshot.contains("lastSeq") &&
            state.snapshot.at("lastSeq").is_number())
        {
            state.last_seq = static_cast<std::uint64_t>(state.snapshot.at("lastSeq").as_int());
            state.acked_seq = state.last_seq;
        }
        if (on_snapshot_)
            on_snapshot_(state.sub_id, state.snapshot);
    }
    return true;
}

bool SubscriptionConsumer::start(std::string& error)
{
    for (SubscriptionState& state : states_)
        if (!subscribe_one(state, std::nullopt, error))
            return false;
    started_ = true;
    return true;
}

bool SubscriptionConsumer::note_incarnation(const std::string& incarnation_id)
{
    if (incarnation_id.empty())
        return false;
    if (incarnation_id_.empty())
    {
        incarnation_id_ = incarnation_id;
        return false;
    }
    if (incarnation_id_ == incarnation_id)
        return false;
    incarnation_id_ = incarnation_id;
    ++stats_.incarnation_changes;
    return true;
}

SubscriptionState* SubscriptionConsumer::find_state(const std::string& sub_id)
{
    for (SubscriptionState& state : states_)
        if (state.sub_id == sub_id)
            return &state;
    return nullptr;
}

void SubscriptionConsumer::apply_event(const std::string& sub_id, const ClientEvent& event)
{
    SubscriptionState* state = find_state(sub_id);
    if (state == nullptr)
        return; // an event for a subscription we already dropped
    // Deltas are strictly ordered: a seq at or below the cursor is a replay of something the
    // snapshot/catch-up already represents, so applying it again would double-count.
    if (event.seq <= state->last_seq)
        return;
    state->last_seq = event.seq;
    ++stats_.events_applied;
    if (on_event_)
        on_event_(sub_id, event);
}

bool SubscriptionConsumer::ack_if_due(SubscriptionState& state, std::string& error, bool force,
                                      bool* rejected_by_daemon)
{
    if (!state.live || state.sub_id.empty())
        return true;
    if (state.last_seq <= state.acked_seq)
        return true;
    if (!force && (state.last_seq - state.acked_seq) < options_.ack_interval)
        return true;

    Json params = Json::object();
    params.set("subId", Json(state.sub_id));
    params.set("seq", Json(state.last_seq));
    if (!client_.call("ack", std::move(params), error, rejected_by_daemon).has_value())
        return false;
    state.acked_seq = state.last_seq;
    ++stats_.acks_sent;
    return true;
}

bool SubscriptionConsumer::flush_acks(std::string& error)
{
    for (SubscriptionState& state : states_)
        if (!ack_if_due(state, error, true))
            return false;
    return true;
}

bool SubscriptionConsumer::resnapshot_all(std::string& error, bool* rejected_by_daemon)
{
    for (SubscriptionState& state : states_)
        if (!subscribe_one(state, std::nullopt, error, rejected_by_daemon))
            return false;
    return true;
}

bool SubscriptionConsumer::reconnect_and_resume(std::string& error)
{
    for (int attempt = 0;;)
    {
        if (options_.backoff.max_attempts > 0 && attempt >= options_.backoff.max_attempts)
        {
            error = "reconnect backoff exhausted after " +
                    std::to_string(options_.backoff.max_attempts) + " attempts: " + error;
            return false;
        }
        const int delay = options_.backoff.delay_for_attempt(attempt);
        std::this_thread::sleep_for(std::chrono::milliseconds(delay));
        // Stop growing the counter once the delay has saturated at max_ms AND nothing is counting
        // it: under max_attempts == 0 ("retry forever") an unbounded ++attempt eventually overflows,
        // which is signed UB. When max_attempts > 0 the counter IS the cap, so it must keep rising.
        if (options_.backoff.max_attempts > 0 || delay < options_.backoff.max_ms)
            ++attempt;

        std::string attempt_error;
        if (!client_.reconnect(attach_, options_.reconnect_timeout_ms, attempt_error))
        {
            error = attempt_error;
            continue;
        }
        ++stats_.reconnects;

        // A reconnect may land on a RESTARTED daemon (a new incarnation, seqs from zero) — resume
        // from the cursor and let the daemon's `gapped` verdict + the incarnation check decide
        // whether a fresh snapshot is required. subscribe_one() handles both.
        const std::string previous_incarnation = incarnation_id_;
        bool ok = true;
        for (SubscriptionState& state : states_)
        {
            state.live = false;
            const std::optional<std::uint64_t> since =
                state.last_seq > 0 ? std::optional<std::uint64_t>(state.last_seq) : std::nullopt;
            if (!subscribe_one(state, since, attempt_error))
            {
                ok = false;
                break;
            }
        }
        if (!ok)
        {
            error = attempt_error;
            continue;
        }
        // A new incarnation invalidates every cursor: re-snapshot the subscriptions that resumed
        // against what is now a different daemon lifetime.
        if (!previous_incarnation.empty() && previous_incarnation != incarnation_id_)
            return resnapshot_all(error);
        return true;
    }
}

bool SubscriptionConsumer::recover_or_fail(bool rejected_by_daemon, std::string& error)
{
    return rejected_by_daemon ? false : reconnect_and_resume(error);
}

bool SubscriptionConsumer::pump(std::string& error)
{
    bool disconnected = false;
    const std::optional<InboundFrame> frame =
        client_.poll_event(options_.poll_timeout_ms, disconnected);

    if (disconnected)
        return reconnect_and_resume(error);

    bool rejected = false;
    if (frame.has_value())
    {
        if (frame->kind == FrameKind::gap)
        {
            // The daemon dropped events for this connection — every subscription's cursor is now
            // untrustworthy, so the ONLY correct recovery is a fresh snapshot for each.
            ++stats_.gaps_recovered;
            if (!resnapshot_all(error, &rejected))
                return recover_or_fail(rejected, error);
        }
        else if (frame->kind == FrameKind::event)
        {
            if (const std::optional<ClientEvent> event = parse_event(frame->event))
            {
                // A daemon restart mid-stream shows up as an event from a NEW incarnation: the seq
                // space restarted, so re-snapshot rather than compare cursors across lifetimes.
                if (note_incarnation(event->incarnation_id))
                {
                    if (!resnapshot_all(error, &rejected))
                        return recover_or_fail(rejected, error);
                }
                else
                {
                    apply_event(frame->sub_id, *event);
                }
            }
        }
    }

    // An ack rides the SAME wire as the poll above, so it can be the first call to notice the peer
    // died — poll_event() reports `disconnected` only when IT touched the wire, and it returns
    // queued frames without one. So a transport failure here is an ordinary disconnect.
    for (SubscriptionState& state : states_)
    {
        rejected = false;
        if (!ack_if_due(state, error, false, &rejected))
            return recover_or_fail(rejected, error);
    }
    return true;
}

void SubscriptionConsumer::stop()
{
    for (SubscriptionState& state : states_)
        unsubscribe_one(state);
    started_ = false;
}

} // namespace context::editor::client
