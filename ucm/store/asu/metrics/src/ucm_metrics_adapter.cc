#include "kv_metrics/ucm_metrics_adapter.h"
#include <atomic>
#include <cstdio>
#include <exception>
#include <mutex>
#include "metrics_api.h"

namespace kv::metrics {
namespace {

struct UcmMetricBinding final : CachedMetric::Binding {
    explicit UcmMetricBinding(const std::string& name) : nativeMetric{name} {}
    UC::Metrics::CachedMetric nativeMetric;
};

void ReportUpdateFailure() noexcept
{
    // Keep diagnostics bounded, including allocation failures in the logging path.
    static std::atomic_flag reported = ATOMIC_FLAG_INIT;
    if (!reported.test_and_set(std::memory_order_relaxed)) {
        std::fputs("KV UCM metrics update failed; the update was skipped.\n", stderr);
    }
}

class UcmKvMetricsAdapter final : public KvMetricsBackend {
public:
    void UpdateStats(CachedMetric& metric, double value) noexcept override
    {
        try {
            auto* binding = static_cast<UcmMetricBinding*>(metric.Resolve(
                [&metric] { return std::make_unique<UcmMetricBinding>(metric.Name()); }));
            // UCM resolves late registration using its own cached ID and epoch.
            // Node labels deliberately aggregate into the base name in this backend.
            UC::Metrics::UpdateStats(binding->nativeMetric, value);
        } catch (...) {
            ReportUpdateFailure();
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
};

}  // namespace

std::shared_ptr<KvMetricsBackend> CreateUcmKvMetricsAdapter()
{
    return std::make_shared<UcmKvMetricsAdapter>();
}

bool EnsureUcmKvMetricsInstalled(std::string* error)
{
    static std::once_flag once;
    static bool installed = false;
    static std::string installError;
    std::call_once(once, [] {
        try {
            installed = InstallBackend(CreateUcmKvMetricsAdapter(), &installError);
        } catch (const std::exception& ex) {
            installError = ex.what();
        }
    });
    if (!installed) {
        if (error != nullptr) { *error = installError; }
        return false;
    }
    if (!IsEnabled()) {
        if (error != nullptr) { *error = "UCM KV metrics were shut down; restart the worker"; }
        return false;
    }
    return true;
}

}  // namespace kv::metrics
