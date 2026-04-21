#include "workspace/WorkspaceSecretStorage.h"

#include <filesystem>
#include <sstream>

#include "platform/AppDirectories.h"
#include "util/TextFileIO.h"
#include "workspace/WorkspaceCommandParsing.h"

namespace microide::workspace {

SecretStorage::SecretStorage() {
  const std::filesystem::path config_dir =
      platform::ResolveAppDirectory(platform::UserDirectoryKind::Config, "microide");
  if (!config_dir.empty()) {
    storage_path_ = config_dir / "secret-store";
    Load();
  }
}
SecretStorage::~SecretStorage() = default;

bool SecretStorage::Store(const std::string& key, const std::string& value) {
  storage_[key] = value;
  return Save();
}

std::optional<std::string> SecretStorage::Retrieve(const std::string& key) const {
  auto it = storage_.find(key);
  if (it == storage_.end()) {
    return std::nullopt;
  }
  return it->second;
}

bool SecretStorage::Contains(const std::string& key) const {
  return storage_.find(key) != storage_.end();
}

bool SecretStorage::Delete(const std::string& key) {
  auto it = storage_.find(key);
  if (it == storage_.end()) {
    return false;
  }
  storage_.erase(it);
  return Save();
}

std::vector<std::string> SecretStorage::Keys() const {
  std::vector<std::string> result;
  for (const auto& [key, _] : storage_) {
    result.push_back(key);
  }
  return result;
}

void SecretStorage::Clear() {
  storage_.clear();
  Save();
}

bool SecretStorage::Load() {
  storage_.clear();
  if (storage_path_.empty()) {
    return false;
  }
  const auto text = util::ReadTextFile(storage_path_);
  if (!text.has_value()) {
    return false;
  }

  std::istringstream stream(*text);
  std::string line;
  while (std::getline(stream, line)) {
    const ParsedCommandLine parsed = ParseCommandLine(line);
    if (parsed.tokens.size() == 3 && parsed.tokens.front().text == "secret") {
      storage_[parsed.tokens[1].text] = parsed.tokens[2].text;
    }
  }
  return true;
}

bool SecretStorage::Save() const {
  if (storage_path_.empty()) {
    return false;
  }

  std::error_code error;
  std::filesystem::create_directories(storage_path_.parent_path(), error);
  if (error) {
    return false;
  }

  std::ostringstream stream;
  for (const auto& [key, value] : storage_) {
    stream << "secret " << QuoteCommandArg(key) << ' ' << QuoteCommandArg(value) << '\n';
  }
  if (!util::WriteTextFileAtomically(storage_path_, stream.str())) {
    return false;
  }

#ifndef _WIN32
  std::filesystem::permissions(
      storage_path_,
      std::filesystem::perms::owner_read | std::filesystem::perms::owner_write,
      std::filesystem::perm_options::replace, error);
#endif
  return true;
}

}  // namespace microide::workspace
