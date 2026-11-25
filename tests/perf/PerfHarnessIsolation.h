#pragma once

#include <filesystem>
#include <string>

namespace microide::tests::perf {

std::filesystem::path EstablishIsolatedAppRoot(bool keep_artifacts, std::string* error = nullptr);

void CleanupIsolatedAppRoot(const std::filesystem::path& root, bool keep);

}
