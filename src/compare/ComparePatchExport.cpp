#include "compare/ComparePatchExport.h"

// Intentionally empty. The display-only compare patch exporter was removed in
// favor of the real unified-diff generator (project::GenerateComparePatch /
// project::GenerateComparePatchForRows). This translation unit is retained so
// the CMake source list remains valid; see ComparePatchExport.h for the
// rationale and the do-not-regress note.
