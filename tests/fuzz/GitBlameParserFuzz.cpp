#include "project/GitBlameService.h"

#include <cstddef>
#include <cstdint>
#include <string_view>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  if (data == nullptr) {
    return 0;
  }
  const std::string_view input(reinterpret_cast<const char*>(data), size);
  (void)microide::project::ParseGitBlameIncrementalOutput(input);
  return 0;
}
