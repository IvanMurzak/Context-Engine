// The M1 incremental derivation graph — see derivation_graph.h.

#include "context/editor/derivation/derivation_graph.h"

#include "context/kernel/event_bus.h"

#include <utility>
#include <vector>

namespace context::editor::derivation
{

using context::editor::filesync::ChangeType;
using context::editor::filesync::ReconcileChange;

DerivationGraph::DerivationGraph(DerivationConfig config, context::kernel::EventBus* bus)
    : config_(config), bus_(bus)
{
    signal_.high_watermark = config_.high_watermark;
}

WriteTicket DerivationGraph::apply(const ReconcileChange& change, std::string_view source_bytes)
{
    WriteTicket ticket;
    ticket.path = change.path;
    ticket.generation_after = generation_ + 1;

    Pending pending;
    if (change.type == ChangeType::removed)
    {
        pending.removal = true;
        ticket.removal = true;
    }
    else
    {
        pending.form = canonical_parse(source_bytes);
        ++parse_invocations_;
        ticket.canonical_hash = pending.form.canonical_hash;
    }

    // Coalesce: the latest write to a path before the next pass wins (one batched pass per burst).
    pending_[change.path] = std::move(pending);
    refresh_signal();
    return ticket;
}

void DerivationGraph::set_visible(std::string_view path, bool visible)
{
    nodes_[std::string(path)].visible = visible;
}

DerivePassResult DerivationGraph::run_pass()
{
    DerivePassResult result;
    result.generation = generation_;
    if (pending_.empty())
    {
        refresh_signal();
        return result;
    }

    const std::uint64_t target_gen = generation_ + 1;
    const bool overloaded = pending_.size() > config_.high_watermark;

    // Select the batch for this pass. Not overloaded: drain everything. Overloaded: load-shed —
    // visible/queried nodes first (never stall a query), then a bounded fill of the rest; the
    // remainder defers to later passes (R-FILE-013).
    std::vector<std::string> batch;
    if (!overloaded)
    {
        batch.reserve(pending_.size());
        for (const auto& [path, _] : pending_)
            batch.push_back(path);
    }
    else
    {
        for (const auto& [path, _] : pending_)
        {
            auto it = nodes_.find(path);
            if (it != nodes_.end() && it->second.visible)
                batch.push_back(path);
        }
        for (const auto& [path, _] : pending_)
        {
            if (batch.size() >= config_.max_batch_per_pass)
                break;
            auto it = nodes_.find(path);
            if (it == nodes_.end() || !it->second.visible)
                batch.push_back(path);
        }
    }

    for (const std::string& path : batch)
    {
        derive_one(path, pending_[path], target_gen, result);
        pending_.erase(path);
    }

    generation_ = target_gen;
    result.generation = target_gen;
    result.deferred = pending_.size();
    refresh_signal();
    publish_pass_event(result);
    return result;
}

void DerivationGraph::derive_one(const std::string& path, const Pending& pending,
                                 std::uint64_t target_gen, DerivePassResult& result)
{
    Node& node = nodes_[path];

    if (pending.removal)
    {
        if (node.alive)
        {
            unreflect(node.canonical_hash);
            world_.destroy(node.entity);
            node.alive = false;
            node.generation = target_gen;
            ++derivations_;
            ++result.nodes_removed;
        }
        // Removing a source with no derived node is a no-op (idempotent).
        return;
    }

    // Content-hash memoization: an unchanged canonical form means the downstream derivation is skipped
    // — this is what makes incremental re-derive recompute ONLY genuinely affected nodes (L-22).
    if (node.alive && node.canonical_hash == pending.form.canonical_hash)
    {
        ++result.nodes_skipped;
        return;
    }

    if (!node.alive)
    {
        node.entity = world_.create();
        node.alive = true;
    }
    else
    {
        unreflect(node.canonical_hash);
    }

    node.canonical_hash = pending.form.canonical_hash;
    node.generation = target_gen;
    world_.add(node.entity, DerivedSource{pending.form.canonical_hash, target_gen});
    reflect(node.canonical_hash);
    ++derivations_;
    ++result.nodes_derived;
}

bool DerivationGraph::reflects_hash(std::uint64_t canonical_hash) const
{
    auto it = reflected_hashes_.find(canonical_hash);
    return it != reflected_hashes_.end() && it->second > 0;
}

std::optional<DerivedSource> DerivationGraph::node(std::string_view path) const
{
    auto it = nodes_.find(std::string(path));
    if (it == nodes_.end() || !it->second.alive)
        return std::nullopt;
    return DerivedSource{it->second.canonical_hash, it->second.generation};
}

void DerivationGraph::reflect(std::uint64_t canonical_hash)
{
    ++reflected_hashes_[canonical_hash];
}

void DerivationGraph::unreflect(std::uint64_t canonical_hash)
{
    auto it = reflected_hashes_.find(canonical_hash);
    if (it == reflected_hashes_.end())
        return;
    if (--it->second == 0)
        reflected_hashes_.erase(it);
}

void DerivationGraph::refresh_signal()
{
    const bool overloaded = pending_.size() > config_.high_watermark;
    const bool transitioned = overloaded != signal_.overloaded;
    signal_.queue_depth = pending_.size();
    signal_.high_watermark = config_.high_watermark;
    signal_.overloaded = overloaded;
    if (transitioned && bus_ != nullptr)
    {
        bus_->publish(BackpressureEvent{signal_.queue_depth, signal_.high_watermark, overloaded});
    }
}

void DerivationGraph::publish_pass_event(const DerivePassResult& result)
{
    if (bus_ == nullptr)
        return;
    bus_->publish(
        DerivationPassEvent{result.generation, result.nodes_derived, signal_.queue_depth});
}

} // namespace context::editor::derivation
