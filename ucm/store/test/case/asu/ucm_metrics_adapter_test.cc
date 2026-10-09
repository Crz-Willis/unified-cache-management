#include "kv_metrics/ucm_metrics_adapter.h"
#include <gtest/gtest.h>
#include <memory>
#include <string>
#include <thread>
#include <vector>
#include "metrics_api.h"

namespace kv::metrics {
namespace {

class UcmKvMetricsAdapterTest : public testing::Test {
protected:
    void SetUp() override
    {
        UC::Metrics::SetUp();
        UC::Metrics::GetAllStatsAndClear();
        backend_ = CreateUcmKvMetricsAdapter();
        ASSERT_TRUE(InstallBackend(backend_));
        // UCM registrations persist in a process, including across --gtest_repeat.
        static unsigned sequence = 0;
        prefix_ = "adapter_test_" + std::to_string(++sequence) + "_";
    }

    void TearDown() override
    {
        Shutdown();
        UC::Metrics::GetAllStatsAndClear();
    }

    std::string Register(const std::string& suffix, const std::string& type,
                         const std::vector<double>& buckets = {})
    {
        const auto name = prefix_ + suffix;
        UC::Metrics::CreateStats(name, type, buckets);
        return name;
    }

    std::string prefix_;
    std::shared_ptr<KvMetricsBackend> backend_;
};

TEST_F(UcmKvMetricsAdapterTest, RoutesSingleUpdatesAndDrainsOnlyOnce)
{
    CachedMetric counter{Register("counter", "counter")};
    CachedMetric gauge{Register("gauge", "gauge")};
    CachedMetric histogram{Register("histogram", "histogram", {1.0, 5.0})};
    UpdateStats(counter, 2.0);
    UpdateStats(counter, 3.0);
    UpdateStats(gauge, 2.0);
    UpdateStats(gauge, 7.0);
    for (const auto value : {0.5, 3.0, 10.0}) { UpdateStats(histogram, value); }

    const auto stats = UC::Metrics::GetAllStatsAndClear();
    EXPECT_DOUBLE_EQ(std::get<0>(stats).at(counter.Name()), 5.0);
    EXPECT_DOUBLE_EQ(std::get<1>(stats).at(gauge.Name()), 7.0);
    const auto& buckets = std::get<2>(stats).at(histogram.Name());
    EXPECT_EQ(buckets.bucketCounts, (std::vector<uint64_t>{1, 1, 1}));
    EXPECT_DOUBLE_EQ(buckets.sum, 13.5);

    const auto drained = UC::Metrics::GetAllStatsAndClear();
    EXPECT_EQ(std::get<0>(drained).count(counter.Name()), 0U);
    EXPECT_EQ(std::get<2>(drained).count(histogram.Name()), 0U);
}

TEST_F(UcmKvMetricsAdapterTest, RoutesBatchAndIgnoresNullEntries)
{
    CachedMetric counter{Register("counter", "counter")};
    CachedMetric gauge{Register("gauge", "gauge")};
    CachedMetric histogram{Register("histogram", "histogram", {1.0})};
    const MetricUpdate updates[] = {
        {counter,   2.0  },
        {nullptr,   100.0},
        {gauge,     7.0  },
        {histogram, 0.5  },
        {counter,   3.0  }
    };
    UpdateStats(updates, std::size(updates));
    UpdateStats(updates, 0);
    UpdateStats(nullptr, 5);
    // Exercise the backend's own guard as well as the facade guard.
    backend_->UpdateStats(nullptr, 5);
    const auto stats = UC::Metrics::GetAllStatsAndClear();
    EXPECT_DOUBLE_EQ(std::get<0>(stats).at(counter.Name()), 5.0);
    EXPECT_DOUBLE_EQ(std::get<1>(stats).at(gauge.Name()), 7.0);
    EXPECT_EQ(std::get<2>(stats).at(histogram.Name()).bucketCounts, (std::vector<uint64_t>{1, 0}));
    EXPECT_DOUBLE_EQ(std::get<2>(stats).at(histogram.Name()).sum, 0.5);
}

TEST_F(UcmKvMetricsAdapterTest, ResolvesSameCachedMetricAfterLateRegistration)
{
    CachedMetric metric{prefix_ + "late"};
    UpdateStats(metric, 100.0);
    EXPECT_EQ(std::get<0>(UC::Metrics::GetAllStatsAndClear()).count(metric.Name()), 0U);
    UC::Metrics::CreateStats(metric.Name(), "counter");
    UpdateStats(metric, 2.0);
    EXPECT_DOUBLE_EQ(std::get<0>(UC::Metrics::GetAllStatsAndClear()).at(metric.Name()), 2.0);
}

TEST_F(UcmKvMetricsAdapterTest, AggregatesNodeHistogramsIntoBaseName)
{
    const auto name = Register("nodes", "histogram", {1.0, 5.0});
    CachedMetric first{name, {{"node_id", "1"}}};
    CachedMetric second{name, {{"node_id", "2"}}};
    EXPECT_FALSE(RegisterMetricLabels(name, first.Labels()));
    EXPECT_FALSE(RegisterMetricLabels(name, second.Labels()));
    UpdateStats(first, 0.5);
    UpdateStats(first, 3.0);
    UpdateStats(second, 4.0);
    UpdateStats(second, 10.0);
    const auto histograms = std::get<2>(UC::Metrics::GetAllStatsAndClear());
    ASSERT_EQ(histograms.size(), 1U);
    ASSERT_EQ(histograms.count(name), 1U);
    EXPECT_EQ(histograms.at(name).bucketCounts, (std::vector<uint64_t>{1, 2, 1}));
    EXPECT_DOUBLE_EQ(histograms.at(name).sum, 17.5);
}

TEST_F(UcmKvMetricsAdapterTest, FlushAndStopLeaveDrainingToUcm)
{
    CachedMetric metric{Register("counter", "counter")};
    UpdateStats(metric, 2.0);
    Flush();
    backend_->Stop();
    UpdateStats(metric, 3.0);
    Shutdown();
    EXPECT_FALSE(IsEnabled());
    EXPECT_DOUBLE_EQ(std::get<0>(UC::Metrics::GetAllStatsAndClear()).at(metric.Name()), 5.0);
}

TEST_F(UcmKvMetricsAdapterTest, DrainsUpdatesFromRetiredThreads)
{
    CachedMetric counter{Register("counter", "counter")};
    CachedMetric histogram{Register("histogram", "histogram", {1.0})};
    std::vector<std::thread> writers;
    for (int i = 0; i < 4; ++i) {
        writers.emplace_back([&] {
            for (int update = 0; update < 250; ++update) {
                const MetricUpdate updates[] = {
                    {counter,   1.0},
                    {histogram, 0.5}
                };
                UpdateStats(updates, std::size(updates));
            }
        });
    }
    for (auto& writer : writers) { writer.join(); }
    const auto stats = UC::Metrics::GetAllStatsAndClear();
    EXPECT_DOUBLE_EQ(std::get<0>(stats).at(counter.Name()), 1000.0);
    EXPECT_EQ(std::get<2>(stats).at(histogram.Name()).bucketCounts,
              (std::vector<uint64_t>{1000, 0}));
    EXPECT_DOUBLE_EQ(std::get<2>(stats).at(histogram.Name()).sum, 500.0);
}

}  // namespace
}  // namespace kv::metrics
