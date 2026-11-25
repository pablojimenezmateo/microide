#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "perf/PerfHarness.h"

namespace microide::tests::perf {

struct Tolerances {
  double p50_percent = 10.0;
  double p95_percent = 20.0;
  double max_percent = 50.0;
  double alloc_p50_percent = 10.0;
  double alloc_p95_percent = 20.0;
  double alloc_max_percent = 50.0;
  double cpu_p50_percent = 10.0;
  double cpu_p95_percent = 20.0;
  double cpu_max_percent = 50.0;
  double rss_mean_percent = 25.0;
  double net_heap_percent = 10.0;
  double phase_alloc_p50_percent = 10.0;
};

struct BaselineRecord {
  std::string scenario_name;
  MetricSet metrics;
  Tolerances tolerances;
  bool has_cpu_metrics = false;
  bool has_rss_metrics = false;
  bool has_net_heap_metrics = false;
  std::vector<PhaseMetricSet> phases;
  bool has_calibration = false;
  std::size_t iterations = 0;
  std::size_t measurement_revision = 1;
  bool timing_is_advisory = false;
  std::string build_config;
  double wall_spread_percent = 0.0;
};

[[nodiscard]] double EffectiveWallTolerance(double declared, double spread_percent);

inline constexpr double kMaxClockNormalizationFactor = 3.0;

struct MetricComparison {
  std::string metric;
  double expected = 0.0;
  double actual = 0.0;
  double tolerance_percent = 0.0;
  bool passed = true;
  double raw_actual = 0.0;
  bool enforced = true;
  std::string note;
};

struct ClockNormalization {
  bool applied = false;
  double factor = 1.0;
  bool clamped = false;
};

struct BaselineComparison {
  std::string scenario_name;
  bool passed = true;
  std::vector<MetricComparison> metrics;
  ClockNormalization clock;
};

struct CalibrationSpread {
  bool valid = false;
  std::uint64_t min_ns = 0;
  std::uint64_t max_ns = 0;
  double ratio = 1.0;
};

inline constexpr double kCalibrationSpreadNoteRatio = 1.10;

CalibrationSpread MeasureCalibrationSpread(const Aggregate& aggregate);
std::string DescribeCalibrationSpread(const CalibrationSpread& spread);

double MetricDeltaPercent(const MetricComparison& metric);

double EnvelopeUsedPercent(const MetricComparison& metric);

inline constexpr double kEnvelopeNoticePercent = 75.0;

BaselineRecord MergeDeterministicMetrics(const BaselineRecord& existing,
                                         const BaselineRecord& fresh);

std::optional<BaselineRecord> LoadBaseline(const std::filesystem::path& path);
bool SaveBaseline(const std::filesystem::path& path, const BaselineRecord& baseline);
BaselineComparison CompareToBaseline(const BaselineRecord& baseline, const Aggregate& aggregate);

}
