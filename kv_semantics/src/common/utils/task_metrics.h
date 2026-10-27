#pragma once

#include "task_context.h"

namespace kv {

// No dynamic labels: operation names remain visible through the UCM adapter.
void RecordClientTaskOutcome(AsuOpType op, const Status& status, double seconds);
void RecordEntryOutcome(AsuOpType op, std::size_t succeeded, std::size_t failed,
                        double completedBytes);
void RecordSubmittedBytes(AsuOpType op, const std::vector<KVBuffer>& entries);

}  // namespace kv
