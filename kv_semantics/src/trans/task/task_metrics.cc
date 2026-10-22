#include "task_metrics.h"

namespace kv {
namespace {
struct OperationMetrics {
    metrics::CachedMetric succeeded;
    metrics::CachedMetric failed;
    metrics::CachedMetric canceled;
    metrics::CachedMetric duration;
};
#define OUTCOME(prefix, op)                                                   \
    OperationMetrics                                                          \
    {                                                                         \
        metrics::CachedMetric{prefix "_" op "_tasks_succeeded_total"},        \
            metrics::CachedMetric{prefix "_" op "_tasks_failed_total"},       \
            metrics::CachedMetric{prefix "_" op "_tasks_canceled_total"},     \
            metrics::CachedMetric{prefix "_" op "_task_e2e_duration_seconds"} \
    }

OperationMetrics* GetOutcome(bool client, AsuOpType op)
{
    static OperationMetrics clientMetrics[] = {
        OUTCOME("kv_client", "query"),       OUTCOME("kv_client", "load"),
        OUTCOME("kv_client", "store"),       OUTCOME("kv_client", "batch_load"),
        OUTCOME("kv_client", "batch_store"), OUTCOME("kv_client", "delete")};
    static OperationMetrics transportMetrics[] = {
        OUTCOME("kv_transport", "query"),       OUTCOME("kv_transport", "load"),
        OUTCOME("kv_transport", "store"),       OUTCOME("kv_transport", "batch_load"),
        OUTCOME("kv_transport", "batch_store"), OUTCOME("kv_transport", "delete"),
        OUTCOME("kv_transport", "keep_alive")};
    const auto index = static_cast<std::size_t>(op);
    if (index >= (client ? std::size(clientMetrics) : std::size(transportMetrics))) {
        return nullptr;
    }
    return client ? &clientMetrics[index] : &transportMetrics[index];
}
#undef OUTCOME

struct EntryMetrics {
    metrics::CachedMetric succeeded;
    metrics::CachedMetric failed;
    metrics::CachedMetric submittedBytes;
    metrics::CachedMetric completedBytes;
};
#define ENTRIES(op)                                                          \
    EntryMetrics                                                             \
    {                                                                        \
        metrics::CachedMetric{"kv_client_" op "_entries_succeeded_total"},   \
            metrics::CachedMetric{"kv_client_" op "_entries_failed_total"},  \
            metrics::CachedMetric{"kv_client_" op "_submitted_bytes_total"}, \
            metrics::CachedMetric{"kv_client_" op "_completed_bytes_total"}  \
    }
EntryMetrics* GetEntries(AsuOpType op)
{
    static EntryMetrics entries[] = {ENTRIES("query"),       ENTRIES("load"),
                                     ENTRIES("store"),       ENTRIES("batch_load"),
                                     ENTRIES("batch_store"), ENTRIES("delete")};
    const auto index = static_cast<std::size_t>(op);
    return index < std::size(entries) ? &entries[index] : nullptr;
}
#undef ENTRIES
}  // namespace

void RecordTaskOutcome(bool client, AsuOpType op, const Status& status, double seconds)
{
    if (!metrics::IsEnabled()) { return; }
    auto* ids = GetOutcome(client, op);
    if (!ids) { return; }
    auto& outcome = status.ok()                           ? ids->succeeded
                    : status.code == StatusCode::CANCELED ? ids->canceled
                                                          : ids->failed;
    const metrics::MetricUpdate updates[] = {
        {outcome,       1.0    },
        {ids->duration, seconds}
    };
    metrics::UpdateStats(updates, std::size(updates));
}

void RecordEntryOutcome(AsuOpType op, std::size_t succeeded, std::size_t failed,
                        double completedBytes)
{
    if (!metrics::IsEnabled()) { return; }
    auto* ids = GetEntries(op);
    if (!ids) { return; }
    const metrics::MetricUpdate updates[] = {
        {ids->succeeded,      static_cast<double>(succeeded)},
        {ids->failed,         static_cast<double>(failed)   },
        {ids->completedBytes, completedBytes                }
    };
    metrics::UpdateStats(updates, IsEntryBatchOp(op) ? 3 : 2);
}

void RecordSubmittedBytes(AsuOpType op, const std::vector<KVBuffer>& entries)
{
    if (!metrics::IsEnabled()) { return; }
    auto* ids = GetEntries(op);
    if (!ids || !IsEntryBatchOp(op)) { return; }
    double bytes = 0;
    for (const auto& entry : entries) { bytes += static_cast<double>(entry.buffer.region.size); }
    metrics::UpdateStats(ids->submittedBytes, bytes);
}
}  // namespace kv
