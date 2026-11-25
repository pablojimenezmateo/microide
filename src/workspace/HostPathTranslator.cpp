#include "workspace/HostPathTranslator.h"

#include <array>
#include <filesystem>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

#include "workspace/FileUri.h"

namespace microide::workspace {

namespace {

constexpr std::array<std::string_view, 7> kUriKeys = {
    "uri", "rootUri", "targetUri", "oldUri", "newUri", "scopeUri", "baseUri",
};
constexpr std::array<std::string_view, 4> kPathKeys = {"rootPath", "path", "program", "cwd"};

template <std::size_t N>
bool Contains(const std::array<std::string_view, N>& keys, std::string_view key) {
  for (const std::string_view candidate : keys) {
    if (candidate == key) {
      return true;
    }
  }
  return false;
}

}

void HostPathTranslator::ToHost(util::JsonValue& message) const {
  if (active()) {
    Walk(message, Direction::kToHost);
  }
}

void HostPathTranslator::FromHost(util::JsonValue& message) const {
  if (active()) {
    Walk(message, Direction::kFromHost);
  }
}

void HostPathTranslator::TranslatePath(std::string& path, Direction direction) const {
  if (path.empty() || path.front() != '/') {
    return;
  }
  std::filesystem::path mapped = direction == Direction::kToHost
                                     ? launcher_->ResolveWorkingDirectory(path)
                                     : launcher_->LocalPathFromHost(path);
  if (mapped != std::filesystem::path(path)) {
    path = mapped.string();
  }
}

void HostPathTranslator::TranslateUri(std::string& uri, Direction direction) const {
  const std::optional<std::filesystem::path> path = PathFromFileUri(uri);
  if (!path.has_value()) {
    return;
  }
  const std::filesystem::path mapped = direction == Direction::kToHost
                                           ? launcher_->ResolveWorkingDirectory(*path)
                                           : launcher_->LocalPathFromHost(*path);
  if (mapped != *path) {
    uri = FileUriForPath(mapped);
  }
}

void HostPathTranslator::Walk(util::JsonValue& value, Direction direction) const {
  if (util::JsonArray* array = value.MutableArray()) {
    for (util::JsonValue& element : *array) {
      Walk(element, direction);
    }
    return;
  }
  util::JsonObject* object = value.MutableObject();
  if (object == nullptr) {
    return;
  }
  for (util::JsonObjectEntry& entry : *object) {
    if (std::string* text = entry.value.MutableString()) {
      if (Contains(kUriKeys, entry.key)) {
        TranslateUri(*text, direction);
      } else if (Contains(kPathKeys, entry.key)) {
        TranslatePath(*text, direction);
      }
      continue;
    }
    if (entry.key == "changes") {
      if (util::JsonObject* changes = entry.value.MutableObject()) {
        util::JsonObject rebuilt;
        rebuilt.reserve(changes->size());
        for (util::JsonObjectEntry& change : *changes) {
          std::string uri = std::move(change.key);
          TranslateUri(uri, direction);
          Walk(change.value, direction);
          rebuilt[uri] = std::move(change.value);
        }
        *changes = std::move(rebuilt);
        continue;
      }
    }
    Walk(entry.value, direction);
  }
}

}
