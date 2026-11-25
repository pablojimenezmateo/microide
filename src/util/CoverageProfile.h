#pragma once

extern "C" {
int __llvm_profile_write_file(void) __attribute__((weak));     // NOLINT(bugprone-reserved-identifier)
void __llvm_profile_reset_counters(void) __attribute__((weak));  // NOLINT(bugprone-reserved-identifier)
}

namespace microide::util {

inline bool CoverageProfileInstrumented() noexcept { return &__llvm_profile_write_file != nullptr; }

inline bool FlushCoverageProfile() noexcept {
  if (&__llvm_profile_write_file == nullptr) {
    return false;
  }
  const bool written = __llvm_profile_write_file() == 0;
  if (written && &__llvm_profile_reset_counters != nullptr) {
    __llvm_profile_reset_counters();
  }
  return written;
}

}
