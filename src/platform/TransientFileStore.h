#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace microide::platform {

// A file handed to a terminal program from somewhere it cannot read -- a file
// dropped on a host terminal from the client's disk (file/upload), an image
// pasted from the clipboard -- kept for a day.
//
// `content` is written as `name` in a fresh directory under `root`, so two files
// of one name never overwrite each other and the file keeps the name a program
// shows. `root` is created private to the user (0700) and must be a real
// directory of theirs: a link planted there does not redirect the write. Entries
// older than a day are removed on the way in, so the store needs no sweeper. The
// file's path, or nullopt and *error.
std::optional<std::filesystem::path> StoreTransientFile(const std::filesystem::path& root,
                                                        const std::string& name, std::string_view content,
                                                        std::string* error);

}  // namespace microide::platform
