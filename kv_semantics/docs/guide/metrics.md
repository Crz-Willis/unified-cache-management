# ASU metrics

ASU uses the `kv_metrics` facade at native client and transport call sites.
Standalone applications can install `SetUpStandaloneMetrics`; UCM's ASU store
installs `kv_metrics_ucm_adapter`, which forwards those same samples to
`UC::Metrics`. The Python dispatcher drains the collector once and distributes
the snapshot to the enabled multiproc and vLLM consumers.

The catalog contains 137 definitions: 97 counters, 10 gauges and 30 histograms.
The original 37 definitions are retained. All duration metrics ending in
`_seconds` use seconds; byte counters use payload bytes.

## Added instrumentation

| Location | Metrics | Meaning |
| --- | --- | --- |
| Client/transport final completion | `kv_{client,transport}_{op}_tasks_{succeeded,failed,canceled}_total`, `..._task_e2e_duration_seconds` | One outcome and duration per completed task, independent of whether the caller invokes Wait |
| Client final completion | `kv_client_{op}_entries_{succeeded,failed}_total` | Entry-level results, including partial failures |
| Accepted client submission / final completion | `kv_client_{op}_{submitted,completed}_bytes_total` | Read/write payload submitted versus successfully completed; only successful entries contribute completed bytes |
| Client worker | `kv_client_prerequisite_wait_duration_seconds`, `kv_client_prerequisite_errors_total` | Prerequisite event synchronization cost and failures |
| Client Wait | `kv_client_wait_timeouts_total` | Caller wait deadline expired; does not claim the underlying task has completed |
| Client/transport queues | `kv_{client,transport}_queue_{depth,capacity}`, `..._queue_full_total`, `..._tasks_inflight` | Aggregate queue pressure, rejection and outstanding accepted tasks |
| Transport buffer pools | `kv_transport_{send,flag}_buffer_slots_{used,capacity}`, `..._buffer_exhaustions_total` | Occupied/available slots and allocation rejection due to exhaustion |
| Transport prepare/send/response | `kv_transport_prepare_failures_total`, `..._send_failed_subbatches_total`, `..._response_parse_errors_total`, `..._cqe_{internal,other}_errors_total` | Additional failure stages; existing connection and timeout counters remain |
| Successful query completion | `kv_client_query_{hit,miss}_keys_total`, `..._prefix_hit_keys` | Key hit/miss counts and distribution of contiguous prefix hits |
| AsuStore lookup | `asu_lookup_timeout_fallback_total` | Timeouts converted into cache misses by the store |
| AsuStore Submit | `asu_build_kv_buffers_{duration_seconds,errors_total}` | Buffer descriptor construction time and failures before native submission |

`op` is `query`, `load`, `store`, `batch_load`, `batch_store` or `delete`.
Transport also has `keep_alive`. Payload byte metrics only apply to the four
read/write operations. UCM AsuStore Load/Dump use `batch_load`/`batch_store`.

A query's `ENTRY_KEY_EXIST` and `ENTRY_KEY_NOT_EXIST` are valid entry results.
Failed queries do not increase normal miss counters. A partially failed task
increases the task failure counter while its successful entries still contribute
to successful entry and byte counters. Canceled tasks have a separate outcome.
Failure-stage counters describe different levels and must not be added together
as a task failure total.

The ten gauges are process-wide sums across ASU clients/transports/pools,
published by one reporter every second. They are sampled values, not high-water
marks. UCM's native per-thread gauge buffers do not preserve chronological order
across writers, so worker threads update atomic accounting instead of publishing
gauges directly. Resource leases release accounting on completion or destruction.
If a timed-out caller abandons its client task, client inflight accounting ends
when that context is destroyed; transport execution can still be outstanding.

## Enable and export

UCM's ASU pipeline preloads the shared UCM metrics library. ASU installs a
native adapter without initializing the UCM collector or registering a catalog.
Metrics configuration is resolved once by the existing UCM setup path:
`enable_metrics`, inline `metrics_config`, `metrics_config_path`, and consumer
selection have the same meaning for ASU as for other stores. There is no
ASU-specific Store switch or connector configuration branch.

Uninitialized collection and omitted metric definitions are silently skipped,
as in the UCM metrics API. Later registration enables subsequent observations;
skipped samples are not replayed. Custom catalogs select which ASU metrics are
collected. Resource reporting starts only when at least one of its ten names
is registered as a Gauge. Registration is checked during ASU setup and on later
native observations, so late registration in an idle process takes effect on
the next observation or Store setup. No resource thread starts for an empty
catalog, a counter-only catalog, or a catalog without resource Gauges.

The collector and adapter are process-wide. A connector with metrics disabled
does not undo another connector's initialization or registration in the same
process. Destroying a store does not stop or drain the shared collector.
Installing a different native backend is rejected; installation and unexpected
update failures are diagnosed without failing storage operations. Runtime
per-Store disabling and configuration hot reload are not supported.

UCM exports use the configured consumer prefix (normally `ucm:` for vLLM).
Standalone native export uses its configured prefix (normally `kv:`).
For example, successful ASU load throughput in UCM can be queried as:

```promql
rate(ucm:kv_client_batch_load_completed_bytes_total[1m])
```

Native node-labeled send/completion histograms remain available in standalone
mode. The UCM adapter currently aggregates node labels into the base metric;
per-node UCM series are not introduced here. These are software-path metrics,
not ASU device capacity, utilization or hardware timing measurements.

## Keep definitions synchronized

The source of truth is `kv_semantics/metrics/config/kv_metrics.yaml`.
After editing it, run from the repository root:

```sh
python kv_semantics/metrics/tools/generate_kv_metrics.py --sync-ucm
python kv_semantics/metrics/tools/generate_kv_metrics.py --check
```

The generator updates the native C++ descriptors and the marked ASU blocks in
`ucm/default_metrics_config.py` and `examples/metrics/metrics_configs.yaml`.
Tests cover partial completion, duplicate notifications, query misses, wait
timeouts, resource aggregation/release, buffer exhaustion, late registration,
consumer gating and consistency of the three catalogs.
