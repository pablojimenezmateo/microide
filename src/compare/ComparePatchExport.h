#pragma once

// The display-only compare patch exporter (FormatCompareHunkPatch /
// FormatCompareFilePatch) was removed: it emitted fake `@@ hunk N @@` headers
// with no real unified line ranges, /dev/null headers, mode lines, or
// no-final-newline markers, so its output could not be fed to `git apply`. The
// copy-to-clipboard path now routes through the real generator in
// project::GenerateComparePatch / project::GenerateComparePatchForRows
// (src/project/PatchGenerator.h), the same one the staging/discard path uses.
//
// This header is intentionally empty; keep it (and its translation unit) so the
// build's source list stays valid. Do not reintroduce a second, weaker patch
// formatter — extend the single generator instead.

namespace microide::compare {}  // namespace microide::compare
