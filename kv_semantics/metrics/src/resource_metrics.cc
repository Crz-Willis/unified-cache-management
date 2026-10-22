#include "kv_metrics/resource_metrics.h"
#include <array>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>
#include "kv_metrics/metrics.h"

namespace kv::metrics {
namespace {
struct Usage {
    std::atomic<std::int64_t> used{0};
    std::atomic<std::int64_t> capacity{0};
};
std::array<Usage, 6> usage;

class Reporter {
    // Own cached handles so they outlive the worker, including during process exit.
    std::array<CachedMetric, 6> used{CachedMetric{"kv_client_queue_depth"},
                                     CachedMetric{"kv_transport_queue_depth"},
                                     CachedMetric{"kv_client_tasks_inflight"},
                                     CachedMetric{"kv_transport_tasks_inflight"},
                                     CachedMetric{"kv_transport_send_buffer_slots_used"},
                                     CachedMetric{"kv_transport_flag_buffer_slots_used"}};
    std::array<CachedMetric, 4> capacity{CachedMetric{"kv_client_queue_capacity"},
                                         CachedMetric{"kv_transport_queue_capacity"},
                                         CachedMetric{"kv_transport_send_buffer_slots_capacity"},
                                         CachedMetric{"kv_transport_flag_buffer_slots_capacity"}};
    void Publish()
    {
        for (std::size_t i = 0; i < used.size(); ++i) {
            UpdateStats(used[i], static_cast<double>(usage[i].used.load()));
        }
        const std::array<std::size_t, 4> indices{0, 1, 4, 5};
        for (std::size_t i = 0; i < capacity.size(); ++i) {
            UpdateStats(capacity[i], static_cast<double>(usage[indices[i]].capacity.load()));
        }
    }

public:
    ~Reporter() { Stop(); }
    bool IsRunning()
    {
        std::lock_guard<std::mutex> lifecycle(lifecycleMutex_);
        return thread_.joinable();
    }
    void Start()
    {
        std::lock_guard<std::mutex> lifecycle(lifecycleMutex_);
        std::lock_guard<std::mutex> lock(mutex_);
        if (thread_.joinable()) { return; }
        stop_ = false;
        thread_ = std::thread([this] {
            std::unique_lock<std::mutex> lock(mutex_);
            do {
                lock.unlock();
                Publish();
                lock.lock();
            } while (!wake_.wait_for(lock, std::chrono::seconds(1), [this] { return stop_; }));
            lock.unlock();
            Publish();
        });
    }
    void Stop()
    {
        std::lock_guard<std::mutex> lifecycle(lifecycleMutex_);
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stop_ = true;
        }
        wake_.notify_all();
        if (thread_.joinable()) { thread_.join(); }
    }

private:
    std::mutex lifecycleMutex_;
    std::mutex mutex_;
    std::condition_variable wake_;
    bool stop_{false};
    std::thread thread_;
};

Reporter& GetReporter()
{
    static Reporter reporter;
    return reporter;
}
}  // namespace

void AdjustResource(Resource resource, std::int64_t used, std::int64_t capacity) noexcept
{
    auto& value = usage[static_cast<std::size_t>(resource)];
    if (used != 0) { value.used.fetch_add(used, std::memory_order_relaxed); }
    if (capacity != 0) { value.capacity.fetch_add(capacity, std::memory_order_relaxed); }
}
void StartResourceReporter() { GetReporter().Start(); }
void StopResourceReporter() { GetReporter().Stop(); }
bool IsResourceReporterRunning() { return GetReporter().IsRunning(); }
}  // namespace kv::metrics
