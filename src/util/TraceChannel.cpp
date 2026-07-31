#include "util/TraceChannel.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <mutex>
#include <unordered_map>

#include "util/StringUtil.h"
#include "util/TransparentStringHash.h"

namespace microide::util {
namespace {

using Clock = std::chrono::steady_clock;

// One slot per *live* channel. Two exist in the app (startup, perf); the cap
// keeps the thread-local active-scope table a fixed-size array instead of a
// per-thread allocation.
//
// Slots are reclaimed on destruction. A monotonic counter would have been
// simpler, but it burns a slot per channel ever constructed, so a test that
// builds a few throwaway channels silently pushes the next one past the cap and
// disables it -- instrumentation that turns itself off is the worst failure mode
// a tracer can have.
constexpr std::size_t kMaxChannels = 8;
constexpr std::size_t kNoSlot = static_cast<std::size_t>(-1);

std::mutex& SlotMutex() {
  static std::mutex mutex;
  return mutex;
}

std::array<bool, kMaxChannels>& SlotsInUse() {
  static std::array<bool, kMaxChannels> in_use{};
  return in_use;
}

std::size_t AcquireSlot() {
  std::lock_guard<std::mutex> lock(SlotMutex());
  std::array<bool, kMaxChannels>& in_use = SlotsInUse();
  for (std::size_t i = 0; i < kMaxChannels; ++i) {
    if (!in_use[i]) {
      in_use[i] = true;
      return i;
    }
  }
  return kNoSlot;
}

void ReleaseSlot(std::size_t slot) {
  if (slot >= kMaxChannels) {
    return;
  }
  std::lock_guard<std::mutex> lock(SlotMutex());
  SlotsInUse()[slot] = false;
}

// Innermost live scope per channel on this thread. A plain thread_local array of
// pointers: no allocation, and nesting depth stays per-thread so a background
// scope can no longer shift the indentation of a main-thread one mid-line.
thread_local std::array<TraceScope*, kMaxChannels> g_active_scope{};

bool ParseEnabledValue(const char* value) {
  if (value == nullptr || value[0] == '\0') {
    return false;
  }
  return !IsFalseyToken(value);
}

bool EnvFlagEnabled(const char* env_name) {
  if (env_name == nullptr || env_name[0] == '\0') {
    return false;
  }
  return ParseEnabledValue(std::getenv(env_name));
}

double ParseMinimumDurationMs(const char* env_name) {
  if (env_name == nullptr || env_name[0] == '\0') {
    return 0.0;
  }
  const char* value = std::getenv(env_name);
  if (value == nullptr || value[0] == '\0') {
    return 0.0;
  }

  char* end = nullptr;
  const double parsed = std::strtod(value, &end);
  if (end == value || !std::isfinite(parsed)) {
    return 0.0;
  }
  return std::max(0.0, parsed);
}

double DurationMs(Clock::duration duration) {
  return std::chrono::duration<double, std::milli>(duration).count();
}

constexpr std::string_view kOverflowLabel = "<aggregate-overflow>";

}  // namespace

struct TraceChannel::Impl {
  mutable std::mutex mutex;
  Clock::time_point origin = Clock::now();
  std::unordered_map<std::string, AggregateEntry, TransparentStringHash, std::equal_to<>>
      aggregate;
};

TraceChannel::TraceChannel(const char* prefix,
                           const char* stream_env,
                           const char* aggregate_env,
                           const char* min_duration_env)
    : prefix_(prefix == nullptr ? "" : prefix),
      stream_enabled_(EnvFlagEnabled(stream_env)),
      aggregate_enabled_(EnvFlagEnabled(aggregate_env)),
      minimum_duration_ms_(ParseMinimumDurationMs(min_duration_env)) {
  if (!Enabled()) {
    // An off channel needs neither a slot nor state; only its enabled flags are
    // ever read.
    slot_ = kNoSlot;
    return;
  }
  slot_ = AcquireSlot();
  if (slot_ == kNoSlot) {
    // More live channels than the thread-local table can index. Degrade to off
    // rather than corrupting another channel's scope stack.
    stream_enabled_ = false;
    aggregate_enabled_ = false;
    std::fprintf(stderr, "[%s] trace channel disabled: more than %zu channels are live\n", prefix_,
                 kMaxChannels);
    return;
  }
  impl_ = new Impl();
}

TraceChannel::~TraceChannel() {
  // Channels are function-local statics, so this runs at exit. Disarm before
  // freeing: any TraceScope constructed after this point (from a later static
  // destructor) must take the disabled fast path rather than touch `impl_`.
  stream_enabled_ = false;
  aggregate_enabled_ = false;
  if (slot_ != kNoSlot) {
    g_active_scope[slot_] = nullptr;
    ReleaseSlot(slot_);
    slot_ = kNoSlot;
  }
  delete impl_;
  impl_ = nullptr;
}

void TraceChannel::Reset() {
  if (impl_ == nullptr) {
    return;
  }
  {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->origin = Clock::now();
  }
  g_active_scope[slot_] = nullptr;
}

void TraceChannel::ResetAggregate() {
  if (impl_ == nullptr) {
    return;
  }
  std::lock_guard<std::mutex> lock(impl_->mutex);
  impl_->aggregate.clear();
}

TraceChannel::Clock::time_point TraceChannel::Origin() const {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  return impl_->origin;
}

void TraceChannel::RecordAggregate(std::string_view label, double total_ms, double self_ms) {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  auto it = impl_->aggregate.find(label);
  if (it == impl_->aggregate.end()) {
    if (impl_->aggregate.size() >= kMaxAggregateLabels) {
      label = kOverflowLabel;
      it = impl_->aggregate.find(label);
    }
    if (it == impl_->aggregate.end()) {
      it = impl_->aggregate.emplace(std::string(label), AggregateEntry{std::string(label)}).first;
    }
  }
  AggregateEntry& entry = it->second;
  ++entry.count;
  entry.total_ms += total_ms;
  entry.self_ms += self_ms;
  entry.max_ms = std::max(entry.max_ms, total_ms);
}

std::vector<TraceChannel::AggregateEntry> TraceChannel::AggregateSnapshot() const {
  std::vector<AggregateEntry> entries;
  if (impl_ == nullptr) {
    return entries;
  }
  {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    entries.reserve(impl_->aggregate.size());
    for (const auto& [label, entry] : impl_->aggregate) {
      entries.push_back(entry);
    }
  }
  std::sort(entries.begin(), entries.end(), [](const AggregateEntry& a, const AggregateEntry& b) {
    if (a.self_ms != b.self_ms) {
      return a.self_ms > b.self_ms;
    }
    return a.label < b.label;
  });
  return entries;
}

void TraceChannel::WriteAggregate(std::FILE* out) const {
  if (out == nullptr) {
    return;
  }
  const std::vector<AggregateEntry> entries = AggregateSnapshot();
  if (entries.empty()) {
    std::fprintf(out, "[%s] summary: no scopes recorded\n", prefix_);
    std::fflush(out);
    return;
  }

  double self_total = 0.0;
  std::uint64_t call_total = 0;
  for (const AggregateEntry& entry : entries) {
    self_total += entry.self_ms;
    call_total += entry.count;
  }

  std::fprintf(out,
               "[%s] summary: %zu labels, %llu calls, %.2f ms self total "
               "(ranked by self ms)\n",
               prefix_, entries.size(), static_cast<unsigned long long>(call_total), self_total);
  std::fprintf(out, "[%s] %12s %12s %12s %12s %10s  %s\n", prefix_, "self ms", "total ms", "max ms",
               "avg ms", "calls", "label");
  for (const AggregateEntry& entry : entries) {
    const double avg_ms = entry.count == 0 ? 0.0 : entry.total_ms / static_cast<double>(entry.count);
    std::fprintf(out, "[%s] %12.3f %12.3f %12.3f %12.4f %10llu  %s\n", prefix_, entry.self_ms,
                 entry.total_ms, entry.max_ms, avg_ms,
                 static_cast<unsigned long long>(entry.count), entry.label.c_str());
  }
  std::fflush(out);
}

void TraceChannel::DumpAggregateOnce() {
  if (!aggregate_enabled_ || dumped_) {
    return;
  }
  dumped_ = true;
  WriteAggregate(stderr);
}

TraceScope::TraceScope(TraceChannel& channel, std::string_view label) {
  if (!channel.Enabled()) {
    return;
  }

  // Copy the label only on the enabled path. These scopes sit in hot paths (the
  // highlight query path constructs several per call), so an unconditional copy
  // here was a per-scope heap allocation in production with tracing off.
  label_.assign(label);
  channel_ = &channel;
  parent_ = g_active_scope[channel.slot_];
  g_active_scope[channel.slot_] = this;
  depth_ = parent_ == nullptr ? 0 : parent_->depth_ + 1;
  start_ = Clock::now();
}

TraceScope::~TraceScope() {
  if (channel_ == nullptr) {
    return;
  }

  const Clock::time_point end = Clock::now();
  const double duration_ms = DurationMs(end - start_);
  g_active_scope[channel_->slot_] = parent_;
  if (parent_ != nullptr) {
    parent_->child_ms_ += duration_ms;
  }

  if (channel_->AggregateEnabled()) {
    channel_->RecordAggregate(label_, duration_ms, std::max(0.0, duration_ms - child_ms_));
  }

  if (!channel_->StreamEnabled() || duration_ms < channel_->MinimumDurationMs()) {
    return;
  }

  const double elapsed_ms = DurationMs(end - channel_->Origin());
  std::fprintf(stderr, "[%s] %8.2f ms total | %8.2f ms | %*s%s\n", channel_->prefix_, elapsed_ms,
               duration_ms, depth_ * 2, "", label_.c_str());
  std::fflush(stderr);
}

}  // namespace microide::util
