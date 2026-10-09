#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

namespace microide::util {

// zstd's "patch-from" (dev-docs/design/remote-projects.md § 6.2): `target`
// compressed with `base` as a raw-content prefix, so the frame holds roughly what
// changed between them. The one-function edit to a 200 KiB file is a few hundred
// bytes. nullopt when zstd refuses (it does not for sane sizes).
std::optional<std::string> ZstdDelta(std::string_view base, std::string_view target);

// The inverse: `target` back from `base` and the frame. nullopt on a malformed or
// truncated frame, a frame for another base (the result is checked by the
// caller's hash), or a result over `max_size`.
std::optional<std::string> ZstdApplyDelta(std::string_view base, std::string_view delta,
                                          std::size_t max_size);

}  // namespace microide::util
