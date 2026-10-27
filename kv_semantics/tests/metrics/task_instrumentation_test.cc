#include <gtest/gtest.h>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include "buffer/buffer_manager.h"
#include "kv_metrics/metrics.h"
#include "kv_metrics/resource_metrics.h"
#include "task/task_manager.h"
#include "task/trans_task_manager.h"
#include "task_metrics.h"

namespace kv::metrics {
namespace {
class Capture final : public KvMetricsBackend {
public:
    void UpdateStats(CachedMetric& metric, double value) noexcept override
    {
        std::lock_guard<std::mutex> lock(mutex);
        samples[metric.Name()].push_back(value);
    }
    void UpdateStats(const MetricUpdate* updates, std::size_t count) noexcept override
    {
        for (std::size_t i = 0; i < count; ++i) {
            if (updates[i].metric) { UpdateStats(*updates[i].metric, updates[i].value); }
        }
    }
    double Sum(const std::string& name)
    {
        std::lock_guard<std::mutex> lock(mutex);
        double sum = 0;
        for (double value : samples[name]) { sum += value; }
        return sum;
    }
    std::size_t Count(const std::string& name)
    {
        std::lock_guard<std::mutex> lock(mutex);
        return samples[name].size();
    }
    double Last(const std::string& name)
    {
        std::lock_guard<std::mutex> lock(mutex);
        return samples[name].empty() ? -1.0 : samples[name].back();
    }
    std::mutex mutex;
    std::map<std::string, std::vector<double>> samples;
};

class TaskInstrumentationTest : public ::testing::Test {
protected:
    void SetUp() override
    {
        Shutdown();
        backend = std::make_shared<Capture>();
        ASSERT_TRUE(InstallBackend(backend));
    }
    void TearDown() override { Shutdown(); }
    ClientTaskPtr Task(AsuOpType op)
    {
        auto task = std::make_shared<ClientTask>();
        task->opType = op;
        task->submittedAt = std::chrono::steady_clock::now() - std::chrono::milliseconds(2);
        task->enqueuedAt = task->submittedAt;
        return task;
    }
    std::shared_ptr<Capture> backend;
};

TEST_F(TaskInstrumentationTest, PartialCompletionCountsOnlySuccessfulPayloadOnce)
{
    auto task = Task(AsuOpType::BATCH_LOAD);
    auto child = std::make_shared<TransportTask>();
    child->entries.resize(3);
    child->entries[0].buffer.region.size = 100;
    child->entries[1].buffer.region.size = 200;
    child->entries[2].buffer.region.size = 400;
    // Deliberately reorder original indices to verify correlation after routing.
    child->originalIndices = {2, 0, 1};
    child->finalStatus = Status::Error(StatusCode::PARTIAL_FAILED, "partial");
    task->transportTasks.push_back(child);
    task->entryStatus = {Status::Error(StatusCode::IO_ERROR, "failed"), Status::OK(), Status::OK()};
    RecordSubmittedBytes(task->opType, child->entries);
    ClientTaskManager::Finalize(task);
    ClientTaskManager::Finalize(task);
    EXPECT_EQ(backend->Sum("kv_client_batch_load_tasks_failed_total"), 1);
    EXPECT_EQ(backend->Sum("kv_client_batch_load_entries_succeeded_total"), 2);
    EXPECT_EQ(backend->Sum("kv_client_batch_load_entries_failed_total"), 1);
    EXPECT_EQ(backend->Sum("kv_client_batch_load_submitted_bytes_total"), 700);
    EXPECT_EQ(backend->Sum("kv_client_batch_load_completed_bytes_total"), 500);
    EXPECT_EQ(backend->Count("kv_client_batch_load_task_e2e_duration_seconds"), 1U);
}

TEST_F(TaskInstrumentationTest, QueryMissIsValidAndFailedQueryDoesNotAddMisses)
{
    auto task = Task(AsuOpType::QUERY);
    task->entryStatus = {Status::Error(StatusCode::ENTRY_KEY_EXIST, "hit"),
                         Status::Error(StatusCode::ENTRY_KEY_NOT_EXIST, "miss"),
                         Status::Error(StatusCode::ENTRY_KEY_EXIST, "hit")};
    task->queryResult.exists = {1, 0, 1};
    ClientTaskManager::Finalize(task);
    EXPECT_EQ(backend->Sum("kv_client_query_tasks_succeeded_total"), 1);
    EXPECT_EQ(backend->Sum("kv_client_query_hit_keys_total"), 2);
    EXPECT_EQ(backend->Sum("kv_client_query_miss_keys_total"), 1);
    EXPECT_EQ(task->queryResult.prefixHitKeys, 1U);

    auto failed = Task(AsuOpType::QUERY);
    failed->entryStatus.resize(4);
    failed->queryResult.exists.resize(4);
    ClientTaskManager::CompleteWithError(failed, Status::Error(StatusCode::TIMEOUT, "timeout"));
    EXPECT_EQ(backend->Sum("kv_client_query_miss_keys_total"), 1);
    EXPECT_EQ(backend->Sum("kv_client_query_hit_keys_total"), 2);
    EXPECT_EQ(backend->Sum("kv_client_query_tasks_failed_total"), 1);
}

TEST_F(TaskInstrumentationTest, CompletionAndCancellationAreIndependentOfWait)
{
    auto task = std::make_shared<TransportTask>();
    task->opType = AsuOpType::STORE;
    task->submittedAt = std::chrono::steady_clock::now();
    task->finalStatus = Status::Error(StatusCode::CANCELED, "shutdown");
    unsigned callbacks = 0;
    task->onComplete = [&](TaskResult) { ++callbacks; };
    TransportTaskManager manager;
    manager.NotifyCompletion(task);
    manager.NotifyCompletion(task);
    EXPECT_EQ(callbacks, 1U);
    EXPECT_EQ(backend->Sum("kv_transport_tasks_canceled_total"), 1);
    EXPECT_EQ(backend->Sum("kv_transport_tasks_failed_total"), 0);
    EXPECT_EQ(backend->Count("kv_transport_task_e2e_duration_seconds"), 1U);

    for (auto op : {AsuOpType::QUERY, AsuOpType::LOAD}) {
        auto failed = std::make_shared<TransportTask>();
        failed->opType = op;
        failed->submittedAt = std::chrono::steady_clock::now();
        failed->finalStatus = Status::Error(StatusCode::PARTIAL_FAILED, "partial");
        manager.NotifyCompletion(failed);
        manager.NotifyCompletion(failed);
    }
    EXPECT_EQ(backend->Sum("kv_transport_tasks_failed_total"), 2);
    EXPECT_EQ(backend->Sum("kv_transport_tasks_canceled_total"), 1);
    EXPECT_EQ(backend->Count("kv_transport_task_e2e_duration_seconds"), 3U);
}

TEST_F(TaskInstrumentationTest, CompletionMetricsDoNotSuppressLateCallback)
{
    auto task = std::make_shared<TransportTask>();
    task->opType = AsuOpType::STORE;
    task->submittedAt = std::chrono::steady_clock::now();
    task->entryStatus = {Status::OK()};
    TransportTaskManager manager;
    TaskId id;
    ASSERT_TRUE(manager.Submit(task, id).ok());
    // With no callback, completion still records metrics and removes the task.
    manager.NotifyCompletion(task);
    EXPECT_EQ(manager.Get(id), nullptr);

    unsigned callbacks = 0;
    task->onComplete = [&](TaskResult result) {
        ++callbacks;
        EXPECT_TRUE(result.status.ok());
        ASSERT_EQ(result.entryStatus.size(), 1U);
        EXPECT_TRUE(result.entryStatus[0].ok());
    };
    // The existing callback guard owns notification, independently of metrics.
    manager.NotifyCompletion(task);
    manager.NotifyCompletion(task);
    EXPECT_EQ(callbacks, 1U);
    EXPECT_EQ(backend->Sum("kv_transport_tasks_failed_total"), 0);
    EXPECT_EQ(backend->Sum("kv_transport_tasks_canceled_total"), 0);
    EXPECT_EQ(backend->Count("kv_transport_task_e2e_duration_seconds"), 1U);
}

TEST_F(TaskInstrumentationTest, WaitTimeoutDoesNotInventTaskCompletion)
{
    auto task = Task(AsuOpType::LOAD);
    ClientTaskManager manager;
    TaskId id;
    ASSERT_TRUE(manager.Submit(task, id).ok());
    TaskResult result;
    EXPECT_EQ(manager.Wait(id, 1, result).code, StatusCode::TIMEOUT);
    EXPECT_EQ(backend->Sum("kv_client_wait_timeouts_total"), 1);
    EXPECT_EQ(backend->Count("kv_client_load_task_e2e_duration_seconds"), 0U);
}

TEST_F(TaskInstrumentationTest, ResourceGaugeAggregatesInstancesAndReleasesExactlyOnce)
{
    {
        ResourceCapacity a(Resource::ClientQueue), b(Resource::ClientQueue);
        a.Set(8);
        b.Set(16);
        ResourceLease first(Resource::ClientQueue), second(Resource::ClientQueue);
        std::thread one([&] {
            first.Acquire();
            first.Acquire();
        });
        std::thread two([&] { second.Acquire(); });
        one.join();
        two.join();
        StartResourceReporter();
        StopResourceReporter();
        EXPECT_EQ(backend->Last("kv_client_queue_depth"), 2);
        EXPECT_EQ(backend->Last("kv_client_queue_capacity"), 24);
        first.Release();
        first.Release();
    }
    StartResourceReporter();
    StopResourceReporter();
    EXPECT_EQ(backend->Last("kv_client_queue_depth"), 0);
    EXPECT_EQ(backend->Last("kv_client_queue_capacity"), 0);
}

TEST_F(TaskInstrumentationTest, BufferExhaustionAndShutdownRestoreCapacity)
{
    {
        BufferManager buffer;
        buffer.SetMetricResource(Resource::SendSlots);
        ASSERT_TRUE(buffer.Init("test", MemoryType::HOST, 64, 1).ok());
        ScatterGatherEntry first, second;
        ASSERT_TRUE(buffer.Allocate(32, first).ok());
        EXPECT_EQ(buffer.Allocate(32, second).code, StatusCode::RESOURCE_BUSY);
        EXPECT_EQ(backend->Sum("kv_transport_send_buffer_exhaustions_total"), 1);
        StartResourceReporter();
        StopResourceReporter();
        EXPECT_EQ(backend->Last("kv_transport_send_buffer_slots_used"), 1);
        EXPECT_EQ(backend->Last("kv_transport_send_buffer_slots_capacity"), 1);
        ASSERT_TRUE(buffer.Free(first.slot_index).ok());
    }
    StartResourceReporter();
    StopResourceReporter();
    EXPECT_EQ(backend->Last("kv_transport_send_buffer_slots_used"), 0);
    EXPECT_EQ(backend->Last("kv_transport_send_buffer_slots_capacity"), 0);
}
}  // namespace
}  // namespace kv::metrics
