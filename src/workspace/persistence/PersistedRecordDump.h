#pragma once

#include <filesystem>
#include <string>

namespace microide::persistence {

bool DumpPersistedRecordFile(const std::filesystem::path& path,
                             std::string* output,
                             std::string* error);

}  // namespace microide::persistence
