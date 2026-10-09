#pragma once

#include <memory>
#include <string>
#include "kv_metrics/metrics.h"

namespace kv::metrics {

// Writes to the collector registered by Python. Does not register or drain metrics.
std::shared_ptr<KvMetricsBackend> CreateUcmKvMetricsAdapter();

// Install once per worker, before starting any KV client. All AsuStore instances
// share the backend, including instances created after a failed Setup or teardown.
// Shutdown/reinstallation and changing backend types within a worker are unsupported.
bool EnsureUcmKvMetricsInstalled(std::string* error = nullptr);

}  // namespace kv::metrics
