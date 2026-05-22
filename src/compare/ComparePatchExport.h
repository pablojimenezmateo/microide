#pragma once

#include <filesystem>
#include <string>

#include "compare/CompareModel.h"

namespace microide::compare {

std::string FormatCompareHunkPatch(const CompareModel& model,
                                   int hunk_index,
                                   const std::filesystem::path& relative_path);
std::string FormatCompareFilePatch(const CompareModel& model,
                                   const std::filesystem::path& relative_path);

}  // namespace microide::compare
