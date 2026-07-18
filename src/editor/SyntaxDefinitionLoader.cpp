#include "editor/SyntaxDefinitionLoader.h"

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_set>
#include <vector>

#include "platform/Filesystem.h"
#include "util/TextFileIO.h"

#if MICROIDE_HAS_LUA_PLUGINS
#include <lua.hpp>

#include "plugin/LuaErrorMessage.h"
#endif

namespace microide::editor::runtime_syntax {

namespace {

#if MICROIDE_HAS_LUA_PLUGINS

using microide::plugin::LuaErrorString;

// Robustness caps: a plugin syntax file is untrusted-ish input evaluated on the
// reload/startup path, so bound every array length it can declare and the total
// instructions its top-level chunk may execute. A malicious or accidental sparse
// table with a huge `#` length, or a `while true do end`, must not hang or
// exhaust memory during a plugin reload.
constexpr std::size_t kMaxDefinitionsPerFile = 256;
constexpr std::size_t kMaxStringArrayEntries = 4096;
constexpr std::size_t kMaxRulesPerArray = 4096;
constexpr int kSyntaxLoadInstructionBudget = 20'000'000;
// Aggregate budgets across ALL discovered files: the per-file caps above bound one
// file, but a plugin dir with thousands of tiny files (or many files each declaring
// definitions) could still turn reload/startup into a large CPU+memory spike and inflate
// the registry that every later file-open/tab-switch scans. Cap the discovered file
// count and the total loaded runtime definitions. (TD-2026-07-16-23.)
constexpr std::size_t kMaxDiscoveredSyntaxFiles = 8192;
constexpr std::size_t kMaxTotalRuntimeDefinitions = 20000;

void SyntaxLoadInstructionGuard(lua_State* state, lua_Debug* /*ar*/) {
  // Fired once the instruction budget is exhausted. Raising a Lua error longjmps
  // back to the enclosing lua_pcall (the project links the C build of Lua and
  // there are no C++ locals on the interpreter frame between them), turning an
  // infinite loop into a clean load failure instead of a frozen UI.
  luaL_error(state, "syntax definition exceeded the instruction budget");
}

bool ReadStringArrayField(lua_State* state,
                          int table_index,
                          const char* field_name,
                          bool required,
                          std::vector<std::string>* values,
                          std::string* error_message) {
  if (values == nullptr) {
    return false;
  }

  values->clear();
  const int absolute_index = lua_absindex(state, table_index);
  lua_getfield(state, absolute_index, field_name);
  if (lua_isnil(state, -1)) {
    lua_pop(state, 1);
    if (required && error_message != nullptr) {
      *error_message = std::string(field_name) + " must be a string or array of strings";
    }
    return !required;
  }

  if (lua_isstring(state, -1)) {
    values->push_back(lua_tostring(state, -1));
    lua_pop(state, 1);
    return true;
  }

  if (!lua_istable(state, -1)) {
    if (error_message != nullptr) {
      *error_message = std::string(field_name) + " must be a string or array of strings";
    }
    lua_pop(state, 1);
    return false;
  }

  const std::size_t count = std::min<std::size_t>(lua_rawlen(state, -1), kMaxStringArrayEntries);
  values->reserve(count);
  for (std::size_t i = 1; i <= count; ++i) {
    lua_rawgeti(state, -1, static_cast<lua_Integer>(i));
    if (!lua_isstring(state, -1)) {
      if (error_message != nullptr) {
        *error_message = std::string(field_name) + " entries must be strings";
      }
      lua_pop(state, 2);
      return false;
    }
    values->push_back(lua_tostring(state, -1));
    lua_pop(state, 1);
  }

  lua_pop(state, 1);
  if (required && values->empty()) {
    if (error_message != nullptr) {
      *error_message = std::string(field_name) + " must not be empty";
    }
    return false;
  }
  return true;
}

bool ReadStringField(lua_State* state,
                     int table_index,
                     const char* field_name,
                     bool required,
                     std::string* value,
                     std::string* error_message) {
  if (value == nullptr) {
    return false;
  }

  value->clear();
  const int absolute_index = lua_absindex(state, table_index);
  lua_getfield(state, absolute_index, field_name);
  if (lua_isnil(state, -1)) {
    lua_pop(state, 1);
    if (required && error_message != nullptr) {
      *error_message = std::string(field_name) + " must be a string";
    }
    return !required;
  }

  if (!lua_isstring(state, -1)) {
    if (error_message != nullptr) {
      *error_message = std::string(field_name) + " must be a string";
    }
    lua_pop(state, 1);
    return false;
  }

  *value = lua_tostring(state, -1);
  lua_pop(state, 1);
  if (required && value->empty()) {
    if (error_message != nullptr) {
      *error_message = std::string(field_name) + " must not be empty";
    }
    return false;
  }
  return true;
}

bool LoadRule(lua_State* state,
              int table_index,
              const std::filesystem::path& source_path,
              RuntimeSyntaxRuleData* rule,
              std::string* error_message);

bool LoadRuleArray(lua_State* state,
                   int table_index,
                   const char* field_name,
                   bool required,
                   const std::filesystem::path& source_path,
                   std::vector<RuntimeSyntaxRuleData>* rules,
                   std::string* error_message) {
  if (rules == nullptr) {
    return false;
  }

  rules->clear();
  const int absolute_index = lua_absindex(state, table_index);
  lua_getfield(state, absolute_index, field_name);
  if (lua_isnil(state, -1)) {
    lua_pop(state, 1);
    if (required && error_message != nullptr) {
      *error_message = std::string(field_name) + " must be an array of rule tables";
    }
    return !required;
  }

  if (!lua_istable(state, -1)) {
    if (error_message != nullptr) {
      *error_message = std::string(field_name) + " must be an array of rule tables";
    }
    lua_pop(state, 1);
    return false;
  }

  const std::size_t count = std::min<std::size_t>(lua_rawlen(state, -1), kMaxRulesPerArray);
  rules->reserve(count);
  for (std::size_t i = 1; i <= count; ++i) {
    lua_rawgeti(state, -1, static_cast<lua_Integer>(i));
    if (!lua_istable(state, -1)) {
      if (error_message != nullptr) {
        *error_message = std::string(field_name) + " entries must be rule tables";
      }
      lua_pop(state, 2);
      return false;
    }

    RuntimeSyntaxRuleData loaded_rule;
    if (!LoadRule(state, -1, source_path, &loaded_rule, error_message)) {
      lua_pop(state, 2);
      return false;
    }
    rules->push_back(std::move(loaded_rule));
    lua_pop(state, 1);
  }

  lua_pop(state, 1);
  if (required && rules->empty()) {
    if (error_message != nullptr) {
      *error_message = std::string(field_name) + " must not be empty";
    }
    return false;
  }
  return true;
}

bool LoadRule(lua_State* state,
              int table_index,
              const std::filesystem::path& source_path,
              RuntimeSyntaxRuleData* rule,
              std::string* error_message) {
  if (rule == nullptr) {
    return false;
  }

  RuntimeSyntaxRuleData loaded_rule;
  if (!ReadStringField(state, table_index, "group", true, &loaded_rule.group_name, error_message)) {
    return false;
  }

  std::string pattern;
  std::string start_regex;
  std::string end_regex;
  if (!ReadStringField(state, table_index, "pattern", false, &pattern, error_message) ||
      !ReadStringField(state, table_index, "start", false, &start_regex, error_message) ||
      !ReadStringField(state, table_index, "end", false, &end_regex, error_message) ||
      !ReadStringField(state, table_index, "skip", false, &loaded_rule.skip_regex, error_message) ||
      !ReadStringField(state, table_index, "limit_group", false, &loaded_rule.limit_group_name,
                       error_message)) {
    return false;
  }

  const bool has_pattern = !pattern.empty();
  const bool has_region_bounds = !start_regex.empty() || !end_regex.empty();
  if (has_pattern == has_region_bounds) {
    if (error_message != nullptr) {
      *error_message = "rules in " + source_path.string() +
                       " must provide either pattern or start/end";
    }
    return false;
  }

  if (has_pattern) {
    loaded_rule.kind = GeneratedRuleKind::Pattern;
    loaded_rule.pattern = std::move(pattern);
  } else {
    if (start_regex.empty() || end_regex.empty()) {
      if (error_message != nullptr) {
        *error_message = "region rules in " + source_path.string() +
                         " must provide both start and end";
      }
      return false;
    }
    loaded_rule.kind = GeneratedRuleKind::Region;
    loaded_rule.start_regex = std::move(start_regex);
    loaded_rule.end_regex = std::move(end_regex);
    if (loaded_rule.limit_group_name.empty()) {
      loaded_rule.limit_group_name = loaded_rule.group_name;
    }
    if (!LoadRuleArray(state, table_index, "rules", false, source_path, &loaded_rule.children,
                       error_message)) {
      return false;
    }
  }

  *rule = std::move(loaded_rule);
  return true;
}

bool LoadDefinition(lua_State* state,
                    int table_index,
                    const std::filesystem::path& source_path,
                    RuntimeSyntaxDefinitionData* definition,
                    std::string* error_message) {
  if (definition == nullptr) {
    return false;
  }

  RuntimeSyntaxDefinitionData loaded_definition;
  loaded_definition.source_path = source_path.lexically_normal();
  if (!ReadStringField(state, table_index, "filetype", true, &loaded_definition.filetype,
                       error_message) ||
      !ReadStringArrayField(state, table_index, "files", false,
                            &loaded_definition.filename_patterns, error_message) ||
      !ReadStringArrayField(state, table_index, "headers", false,
                            &loaded_definition.header_patterns, error_message) ||
      !ReadStringArrayField(state, table_index, "signatures", false,
                            &loaded_definition.signature_patterns, error_message) ||
      !LoadRuleArray(state, table_index, "rules", true, source_path, &loaded_definition.rules,
                     error_message)) {
    return false;
  }

  if (loaded_definition.filename_patterns.empty() && loaded_definition.header_patterns.empty()) {
    if (error_message != nullptr) {
      *error_message = "syntax definition in " + source_path.string() +
                       " must declare files or headers";
    }
    return false;
  }

  *definition = std::move(loaded_definition);
  return true;
}

bool LoadDefinitionFile(const std::filesystem::path& source_path,
                        std::vector<RuntimeSyntaxDefinitionData>* definitions,
                        std::string* error_message) {
  if (definitions == nullptr) {
    return false;
  }

  lua_State* state = luaL_newstate();
  if (state == nullptr) {
    if (error_message != nullptr) {
      *error_message = "failed to create Lua state for " + source_path.string();
    }
    return false;
  }

  const auto close_state = [&state]() {
    if (state != nullptr) {
      lua_close(state);
      state = nullptr;
    }
  };

  if (luaL_loadfile(state, source_path.string().c_str()) != LUA_OK) {
    if (error_message != nullptr) {
      *error_message = "failed to load syntax definition " + source_path.string() + ": " +
                       LuaErrorString(state);
    }
    close_state();
    return false;
  }

  // Bound the top-level chunk's execution so a runaway loop cannot hang the
  // reload/startup path. The hook fires every kSyntaxLoadInstructionBudget
  // instructions and raises, which lua_pcall catches below.
  lua_sethook(state, SyntaxLoadInstructionGuard, LUA_MASKCOUNT, kSyntaxLoadInstructionBudget);
  const int pcall_status = lua_pcall(state, 0, 1, 0);
  lua_sethook(state, nullptr, 0, 0);
  if (pcall_status != LUA_OK) {
    if (error_message != nullptr) {
      *error_message = "failed to evaluate syntax definition " + source_path.string() + ": " +
                       LuaErrorString(state);
    }
    close_state();
    return false;
  }

  if (!lua_istable(state, -1)) {
    if (error_message != nullptr) {
      *error_message = "syntax definition " + source_path.string() +
                       " must return a table or array of tables";
    }
    close_state();
    return false;
  }

  const int root_index = lua_absindex(state, -1);
  lua_getfield(state, root_index, "filetype");
  const bool is_single_definition = lua_isstring(state, -1);
  lua_pop(state, 1);

  if (is_single_definition) {
    RuntimeSyntaxDefinitionData definition;
    const bool loaded = LoadDefinition(state, root_index, source_path, &definition, error_message);
    close_state();
    if (loaded) {
      definitions->push_back(std::move(definition));
    }
    return loaded;
  }

  const std::size_t count =
      std::min<std::size_t>(lua_rawlen(state, root_index), kMaxDefinitionsPerFile);
  if (count == 0) {
    if (error_message != nullptr) {
      *error_message = "syntax definition " + source_path.string() +
                       " must return a table or array of tables";
    }
    close_state();
    return false;
  }

  for (std::size_t i = 1; i <= count; ++i) {
    lua_rawgeti(state, root_index, static_cast<lua_Integer>(i));
    if (!lua_istable(state, -1)) {
      if (error_message != nullptr) {
        *error_message = "syntax definition array in " + source_path.string() +
                         " must contain only tables";
      }
      lua_pop(state, 1);
      close_state();
      return false;
    }

    RuntimeSyntaxDefinitionData definition;
    if (!LoadDefinition(state, -1, source_path, &definition, error_message)) {
      lua_pop(state, 1);
      close_state();
      return false;
    }
    definitions->push_back(std::move(definition));
    lua_pop(state, 1);
  }

  close_state();
  return true;
}

std::vector<std::filesystem::path> DiscoverDefinitionFiles(
    const std::vector<std::filesystem::path>& directories) {
  std::vector<std::filesystem::path> files;
  // Dedup files across directories: a syntax directory contributed via two
  // routes (or two directory entries that normalize to the same path) must load,
  // hash, and compile each definition exactly once, with deterministic
  // first-directory-wins precedence.
  std::unordered_set<std::string> seen_directories;
  std::unordered_set<std::string> seen_files;
  for (const auto& directory : directories) {
    if (!seen_directories.insert(directory.lexically_normal().generic_string()).second) {
      continue;
    }
    if (platform::ReadPathType(directory) != platform::PathType::Directory) {
      continue;
    }

    std::vector<std::filesystem::path> directory_files;
    for (const auto& entry : platform::ListDirectory(directory)) {
      if (entry.type != platform::PathType::RegularFile) {
        continue;
      }
      const std::filesystem::path path = entry.path.lexically_normal();
      if (path.extension() == ".lua" && seen_files.insert(path.generic_string()).second) {
        directory_files.push_back(path);
      }
    }
    std::sort(directory_files.begin(), directory_files.end());
    files.insert(files.end(), directory_files.begin(), directory_files.end());
    if (files.size() >= kMaxDiscoveredSyntaxFiles) {
      files.resize(kMaxDiscoveredSyntaxFiles);
      break;  // aggregate discovered-file budget reached
    }
  }
  return files;
}

std::uint64_t Fnv1aAppend(std::uint64_t hash, std::string_view text) {
  constexpr std::uint64_t kOffsetBasis = 1469598103934665603ULL;
  constexpr std::uint64_t kPrime = 1099511628211ULL;
  if (hash == 0) {
    hash = kOffsetBasis;
  }
  for (const unsigned char byte : text) {
    hash ^= static_cast<std::uint64_t>(byte);
    hash *= kPrime;
  }
  return hash;
}

#endif

}  // namespace

std::vector<RuntimeSyntaxDefinitionData> LoadDefinitionsFromDirectories(
    const std::vector<std::filesystem::path>& directories,
    std::vector<std::string>* errors) {
  std::vector<RuntimeSyntaxDefinitionData> definitions;
  if (errors != nullptr) {
    errors->clear();
  }

#if MICROIDE_HAS_LUA_PLUGINS
  for (const auto& path : DiscoverDefinitionFiles(directories)) {
    // Aggregate runtime-definition budget: stop once the total across all files reaches
    // the cap so a plugin spreading hundreds of thousands of definitions across many
    // files cannot inflate the registry that every later file-open scans. Surface a
    // diagnostic so the truncated plugin is visible rather than a silently missing
    // language. (TD-2026-07-16-23.)
    if (definitions.size() >= kMaxTotalRuntimeDefinitions) {
      if (errors != nullptr) {
        errors->push_back("syntax definitions truncated at the aggregate limit (" +
                          std::to_string(kMaxTotalRuntimeDefinitions) + ")");
      }
      break;
    }
    std::string error_message;
    if (!LoadDefinitionFile(path, &definitions, &error_message) && errors != nullptr &&
        !error_message.empty()) {
      errors->push_back(std::move(error_message));
    }
  }
  // A single file can push slightly past the cap (up to kMaxDefinitionsPerFile); trim.
  if (definitions.size() > kMaxTotalRuntimeDefinitions) {
    definitions.resize(kMaxTotalRuntimeDefinitions);
  }
#else
  (void)directories;
#endif

  return definitions;
}

std::uint64_t SyntaxSourceFingerprint::Compute(
    const std::vector<std::filesystem::path>& directories) {
#if !MICROIDE_HAS_LUA_PLUGINS
  (void)directories;
  return 0;
#else
  // Guard the cache so Compute can run on the plugin worker (TD-2026-07-17A-108).
  std::lock_guard<std::mutex> lock(mutex_);
  std::uint64_t fingerprint = 0;
  for (const auto& path : DiscoverDefinitionFiles(directories)) {
    fingerprint = Fnv1aAppend(fingerprint, path.generic_string());
    fingerprint = Fnv1aAppend(fingerprint, "\n");

    std::error_code mtime_ec;
    std::error_code size_ec;
    const std::filesystem::file_time_type mtime =
        std::filesystem::last_write_time(path, mtime_ec);
    const std::uintmax_t size = std::filesystem::file_size(path, size_ec);
    const std::string key = path.generic_string();

    std::uint64_t content_hash = 0;
    const auto cached = cache_.find(key);
    if (!mtime_ec && !size_ec && cached != cache_.end() &&
        cached->second.mtime == mtime && cached->second.size == size) {
      // Unchanged since the last compute: reuse the content hash, no re-read.
      content_hash = cached->second.content_hash;
    } else {
      const std::optional<std::string> content = util::ReadTextFile(path);
      content_hash = content.has_value() ? Fnv1aAppend(0, *content)
                                         : Fnv1aAppend(0, "<unreadable>");
      if (!mtime_ec && !size_ec) {
        cache_[key] = Entry{mtime, size, content_hash};
      } else {
        // Couldn't stat the file: don't cache, so the next compute re-reads it.
        cache_.erase(key);
      }
    }
    fingerprint = Fnv1aAppend(
        fingerprint,
        std::string_view(reinterpret_cast<const char*>(&content_hash), sizeof(content_hash)));
    fingerprint = Fnv1aAppend(fingerprint, "\n");
  }
  return fingerprint;
#endif
}

}  // namespace microide::editor::runtime_syntax
