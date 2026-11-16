#pragma once

// The source-coverage runtime of a clang `-fprofile-instr-generate` build. Weak:
// in every other build the two are absent and their addresses null.
extern "C" {
int __llvm_profile_write_file(void) __attribute__((weak));     // NOLINT(bugprone-reserved-identifier)
void __llvm_profile_reset_counters(void) __attribute__((weak));  // NOLINT(bugprone-reserved-identifier)
}

namespace microide::util {

// The soak measures which of the app's code its jobs execute with a clang
// source-coverage build (tools/soak/codecov.py). That runtime writes its
// counters from an exit handler -- and the app never runs one: it leaves through
// std::quick_exit once its state is saved, and the QA driver's kills are
// SIGKILLs. The first measured run held the counters of the control clients
// alone: 84 functions of the whole app. These write the counters where the app
// actually ends.

// True in a build that carries the coverage runtime.
inline bool CoverageProfileInstrumented() noexcept { return &__llvm_profile_write_file != nullptr; }

// Writes the counters to the profile (LLVM_PROFILE_FILE) and zeroes them, so a
// later write -- the same process flushing again, the merge pool's next writer
// -- adds only what ran since. False, and nothing done, in any other build.
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

}  // namespace microide::util
