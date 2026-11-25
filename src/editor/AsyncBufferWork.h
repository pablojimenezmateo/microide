#pragma once

#include <atomic>
#include <cstdint>

namespace microide::editor {

class AsyncBufferWork {
 public:
  enum class Claim {
    NotMine,
    Stale,
    Current,
  };

  static std::uint64_t NextId() {
    static std::atomic<std::uint64_t> next_id{0};
    return next_id.fetch_add(1, std::memory_order_relaxed) + 1;
  }

  void Arm(std::uint64_t id, std::uint64_t content_revision) {
    id_ = id;
    posted_revision_ = content_revision;
  }

  std::uint64_t Post(std::uint64_t content_revision) {
    const std::uint64_t id = NextId();
    Arm(id, content_revision);
    return id;
  }

  void Disarm() {
    id_ = 0;
    posted_revision_ = 0;
  }

  [[nodiscard]] bool armed() const { return id_ != 0; }
  [[nodiscard]] std::uint64_t id() const { return id_; }

  [[nodiscard]] bool Holds(std::uint64_t id) const { return id != 0 && id_ == id; }

  [[nodiscard]] Claim Resolve(std::uint64_t id, std::uint64_t current_content_revision) {
    if (!Holds(id)) {
      return Claim::NotMine;
    }
    const bool current = posted_revision_ == current_content_revision;
    Disarm();
    return current ? Claim::Current : Claim::Stale;
  }

 private:
  std::uint64_t id_ = 0;
  std::uint64_t posted_revision_ = 0;
};

}
