#include "kv_metrics/ucm_metrics_adapter.h"
#include <atomic>
#include <chrono>
#include <cstdio>
#include <exception>
#include <mutex>
#include "kv_metrics/resource_metrics.h"
#include "logger.h"
#include "metrics_api.h"

namespace kv::metrics {
namespace {

struct UcmMetricBinding final : CachedMetric::Binding {
    explicit UcmMetricBinding(const std::string& name)
        : CachedMetric::Binding(Kind::Ucm), nativeMetric{name}
    {
    }
    UC::Metrics::CachedMetric nativeMetric;
};

void ReportLoggingFailure() noexcept
{
    // Allocation-free fallback if the normal logger also fails (e.g. bad_alloc).
    // Keep it periodic, but bounded even during sustained memory pressure.
    static std::atomic<int64_t> nextReport{0};
    const auto now = std::chrono::duration_cast<std::chrono::seconds>(
                         std::chrono::steady_clock::now().time_since_epoch())
                         .count();
    auto next = nextReport.load(std::memory_order_relaxed);
    if (now >= next &&
        nextReport.compare_exchange_strong(next, now + 60, std::memory_order_relaxed)) {
        std::fputs("KV UCM metrics failure; diagnostic logging also failed.\n", stderr);
    }
}

void ReportUpdateFailure(const char* metric, const char* reason) noexcept
{
    try {
        UC_ERROR("KV UCM metric update failed for {}; sample skipped: {}", metric, reason);
    } catch (...) {
        ReportLoggingFailure();
    }
}

bool InstallationFailed(std::string* error, const char* reason) noexcept
{
    try {
        UC_ERROR(
            "Failed to install ASU UCM metrics backend: {}. "
            "Continuing without UCM metrics installation; existing backend unchanged. "
            "A later Store Setup can retry.",
            reason);
    } catch (...) {
        ReportLoggingFailure();
    }
    // Even diagnostic string allocation must not make Store initialization fail.
    try {
        if (error != nullptr) { *error = reason; }
    } catch (...) {
    }
    return false;
}

class UcmKvMetricsAdapter final : public KvMetricsBackend {
public:
    void RefreshResourceReporter(bool force = false)
    {
        // Registration only grows. Once started, the reporter needs no further
        // catalog checks. This also keeps its own samples out of the start path.
        if (!force && reporterStarted_.load(std::memory_order_acquire)) { return; }
        const auto epoch = UC::Metrics::GetRegistrationEpoch();
        if (epoch == 0 || (!force && checkedEpoch_.load(std::memory_order_acquire) == epoch)) {
            return;
        }
        std::lock_guard<std::mutex> lock(reporterMutex_);
        if (!force && (reporterStarted_.load(std::memory_order_acquire) ||
                       checkedEpoch_.load(std::memory_order_acquire) == epoch)) {
            return;
        }
        static const std::vector<std::string> gauges{"kv_client_queue_depth",
                                                     "kv_transport_queue_depth",
                                                     "kv_client_queue_capacity",
                                                     "kv_transport_queue_capacity",
                                                     "kv_client_tasks_inflight",
                                                     "kv_transport_tasks_inflight",
                                                     "kv_transport_send_buffer_slots_used",
                                                     "kv_transport_send_buffer_slots_capacity",
                                                     "kv_transport_flag_buffer_slots_used",
                                                     "kv_transport_flag_buffer_slots_capacity"};
        if (UC::Metrics::HasRegisteredGauge(gauges)) {
            reporterStarted_.store(true, std::memory_order_release);
            try {
                StartResourceReporter();
            } catch (...) {
                reporterStarted_.store(false, std::memory_order_release);
                throw;
            }
        }
        checkedEpoch_.store(epoch, std::memory_order_release);
    }

    void UpdateStats(CachedMetric& metric, double value) noexcept override
    {
        try {
            RefreshResourceReporter();
            // Disabled/unconfigured collection is a normal UCM state. Avoid
            // binding allocations until the shared collector is initialized.
            if (UC::Metrics::GetRegistrationEpoch() == 0) { return; }
            auto* cached = metric.Resolve(
                [&metric] { return std::make_unique<UcmMetricBinding>(metric.Name()); });
            if (cached == nullptr || cached->GetKind() != CachedMetric::Binding::Kind::Ucm) {
                ReportUpdateFailure(metric.Name().c_str(), "incompatible cached metric binding");
                return;
            }
            auto* binding = static_cast<UcmMetricBinding*>(cached);
            // UCM resolves late registration using its own cached ID and epoch.
            // Node labels deliberately aggregate into the base name in this backend.
            UC::Metrics::UpdateStats(binding->nativeMetric, value);
        } catch (const std::exception& ex) {
            ReportUpdateFailure(metric.Name().c_str(), ex.what());
        } catch (...) {
            ReportUpdateFailure(metric.Name().c_str(), "unknown exception");
        }
    }

    void UpdateStats(const MetricUpdate* updates, std::size_t count) noexcept override
    {
        if (updates == nullptr) { return; }
        for (std::size_t i = 0; i < count; ++i) {
            if (updates[i].metric != nullptr) { UpdateStats(*updates[i].metric, updates[i].value); }
        }
    }

    bool RegisterMetricLabels(const std::string&, const MetricLabels&) noexcept override
    {
        return false;
    }

    // Flush and Stop inherit no-ops. Only the Python dispatcher owns draining.

private:
    std::atomic<uint64_t> checkedEpoch_{0};
    std::atomic<bool> reporterStarted_{false};
    std::mutex reporterMutex_;
};

}  // namespace

std::shared_ptr<KvMetricsBackend> CreateUcmKvMetricsAdapter()
{
    return std::make_shared<UcmKvMetricsAdapter>();
}

bool EnsureUcmKvMetricsInstalled(std::string* error) noexcept
{
    try {
        static std::mutex mutex;
        // Retain the same adapter across quiescent shutdown/reinstallation.
        // Its cached bindings remain valid across Store lifetimes.
        static std::shared_ptr<UcmKvMetricsAdapter> adapter;
        std::lock_guard<std::mutex> lock{mutex};
        if (error != nullptr) { error->clear(); }
        if (!adapter) { adapter = std::make_shared<UcmKvMetricsAdapter>(); }
        std::string installError;
        if (!IsBackendInstalled(adapter.get()) && !InstallBackend(adapter, &installError)) {
            return InstallationFailed(error, installError.c_str());
        }
        // A retained adapter may be reinstalled after a quiescent Shutdown.
        adapter->RefreshResourceReporter(true);
        return true;
    } catch (const std::exception& ex) {
        return InstallationFailed(error, ex.what());
    } catch (...) {
        return InstallationFailed(error, "unknown exception");
    }
}

}  // namespace kv::metrics
