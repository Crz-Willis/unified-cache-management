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

OperationMetrics* GetOutcome(AsuOpType op)
{
    static OperationMetrics clientMetrics[] = {
        OUTCOME("kv_client", "query"),       OUTCOME("kv_client", "load"),
        OUTCOME("kv_client", "store"),       OUTCOME("kv_client", "batch_load"),
        OUTCOME("kv_client", "batch_store"), OUTCOME("kv_client", "delete")};
    const auto index = static_cast<std::size_t>(op);
    return index < std::size(clientMetrics) ? &clientMetrics[index] : nullptr;
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
    static EntryMetrics entries[] = {ENTRIES("load"), ENTRIES("store"), ENTRIES("batch_load"),
                                     ENTRIES("batch_store")};
    switch (op) {
        case AsuOpType::LOAD: return &entries[0];
        case AsuOpType::STORE: return &entries[1];
        case AsuOpType::BATCH_LOAD: return &entries[2];
        case AsuOpType::BATCH_STORE: return &entries[3];
        default: return nullptr;
    }
}
#undef ENTRIES
}  // namespace

void RecordClientTaskOutcome(AsuOpType op, const Status& status, double seconds)
{
    if (!metrics::IsEnabled()) { return; }
    auto* ids = GetOutcome(op);
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
    metrics::UpdateStats(updates, std::size(updates));
}

void RecordSubmittedBytes(AsuOpType op, const std::vector<KVBuffer>& entries)
{
    if (!metrics::IsEnabled()) { return; }
    auto* ids = GetEntries(op);
    if (!ids) { return; }
    double bytes = 0;
    for (const auto& entry : entries) { bytes += static_cast<double>(entry.buffer.region.size); }
    metrics::UpdateStats(ids->submittedBytes, bytes);
}
}  // namespace kv
