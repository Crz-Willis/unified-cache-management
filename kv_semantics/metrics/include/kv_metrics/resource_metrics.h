#pragma once

#include <atomic>
#include <cstdint>

namespace kv::metrics {

enum class Resource {
    ClientQueue,
    TransportQueue,
    ClientTasks,
    TransportTasks,
    SendSlots,
    FlagSlots
};

// Process-wide sums across all clients/nodes. A single reporter writes the gauges,
// because UCM's thread buffers do not order gauge updates from different writers.
void AdjustResource(Resource resource, std::int64_t used, std::int64_t capacity = 0) noexcept;
void StartResourceReporter();
void StopResourceReporter();
bool IsResourceReporterRunning();

class ResourceLease {
public:
    explicit ResourceLease(Resource resource) : resource_(resource) {}
    ~ResourceLease() { Release(); }
    void Acquire() noexcept
    {
        if (!held_.exchange(true)) { AdjustResource(resource_, 1); }
    }
    void Release() noexcept
    {
        if (held_.exchange(false)) { AdjustResource(resource_, -1); }
    }

private:
    Resource resource_;
    std::atomic<bool> held_{false};
};

class ResourceCapacity {
public:
    explicit ResourceCapacity(Resource resource) : resource_(resource) {}
    ~ResourceCapacity() { Set(0); }
    void Set(std::int64_t capacity) noexcept
    {
        AdjustResource(resource_, 0, capacity - capacity_);
        capacity_ = capacity;
    }

private:
    Resource resource_;
    std::int64_t capacity_{0};
};

}  // namespace kv::metrics
