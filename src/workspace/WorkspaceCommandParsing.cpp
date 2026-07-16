#include "workspace/WorkspaceCommandParsing.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <utility>

#include "util/Parse.h"

namespace microide::workspace {

std::string UiScaleLabel(float scale) {
  const int percent = static_cast<int>(std::lround(scale * 100.0f));
  return std::to_string(percent) + "%";
}

std::optional<float> ParseUiScaleValue(std::string_view text) {
  if (text.empty()) {
    return std::nullopt;
  }

  bool percent = false;
  if (text.back() == '%') {
    percent = true;
    text.remove_suffix(1);
  }
  if (text.empty()) {
    return std::nullopt;
  }

  const std::optional<float> parsed = util::ParseFloat(text);
  if (!parsed.has_value()) {
    return std::nullopt;
  }
  float scale = *parsed;
  if (!percent && scale > 10.0f) {
    scale *= 0.01f;
  }
  if (percent) {
    scale *= 0.01f;
  }
  return std::clamp(scale, kMinUiScale, kMaxUiScale);
}

float StepUiScale(float current_scale, int delta) {
  const float clamped_current = std::clamp(current_scale, kMinUiScale, kMaxUiScale);
  if (delta > 0) {
    for (float candidate : kUiScalePresets) {
      if (candidate > clamped_current + 0.001f) {
        return candidate;
      }
    }
    return kMaxUiScale;
  }
  if (delta < 0) {
    for (auto it = kUiScalePresets.rbegin(); it != kUiScalePresets.rend(); ++it) {
      if (*it < clamped_current - 0.001f) {
        return *it;
      }
    }
    return kMinUiScale;
  }
  return clamped_current;
}

ParsedCommandLine ParseCommandLine(std::string_view input) {
  ParsedCommandLine parsed;
  // Bound the parse: a pasted megabyte command or thousands of quoted tokens must
  // not allocate/drive completion work without limit on the UI thread. The prompt
  // is a single command line — real ones are tiny — so clamp the scanned length
  // and the token count well above any legitimate input.
  constexpr std::size_t kMaxCommandInputBytes = 64 * 1024;
  constexpr std::size_t kMaxCommandTokens = 4096;
  if (input.size() > kMaxCommandInputBytes) {
    input = input.substr(0, kMaxCommandInputBytes);
  }
  std::string current;
  std::size_t token_start = 0;
  bool token_active = false;
  char quote = '\0';
  bool escaping = false;

  const auto begin_token = [&](std::size_t start) {
    if (!token_active) {
      token_active = true;
      token_start = start;
    }
  };
  const auto flush_token = [&]() {
    if (!token_active) {
      return;
    }
    if (parsed.tokens.size() < kMaxCommandTokens) {
      parsed.tokens.push_back(ParsedCommandToken{current, token_start});
    }
    current.clear();
    token_active = false;
  };

  for (std::size_t i = 0; i < input.size(); ++i) {
    const char c = input[i];
    if (escaping) {
      begin_token(i > 0 ? i - 1 : 0);
      current.push_back(c);
      escaping = false;
      parsed.trailing_space = false;
      continue;
    }

    if (quote == '\'') {
      parsed.trailing_space = false;
      if (c == '\'') {
        quote = '\0';
      } else {
        current.push_back(c);
      }
      continue;
    }

    if (quote == '"') {
      parsed.trailing_space = false;
      if (c == '"') {
        quote = '\0';
        continue;
      }
      if (c == '\\') {
        begin_token(token_active ? token_start : i);
        escaping = true;
        continue;
      }
      current.push_back(c);
      continue;
    }

    if (std::isspace(static_cast<unsigned char>(c))) {
      flush_token();
      parsed.trailing_space = true;
      continue;
    }

    parsed.trailing_space = false;
    if (c == '\\') {
      begin_token(i);
      escaping = true;
      continue;
    }
    if (c == '\'' || c == '"') {
      begin_token(i);
      quote = c;
      continue;
    }

    begin_token(i);
    current.push_back(c);
  }

  if (token_active) {
    flush_token();
  }
  parsed.dangling_escape = escaping;
  parsed.open_quote = quote;
  return parsed;
}

std::string CommonPrefix(const std::vector<CommandCompletionCandidate>& candidates) {
  if (candidates.empty()) {
    return {};
  }

  std::string prefix = candidates.front().value;
  for (std::size_t i = 1; i < candidates.size() && !prefix.empty(); ++i) {
    const std::string& value = candidates[i].value;
    std::size_t length = 0;
    while (length < prefix.size() && length < value.size() && prefix[length] == value[length]) {
      ++length;
    }
    prefix.resize(length);
  }
  return prefix;
}

bool CommandArgNeedsQuoting(std::string_view argument) {
  if (argument.empty()) {
    return true;
  }

  return std::any_of(argument.begin(), argument.end(), [](unsigned char c) {
    return !std::isalnum(c) && c != '_' && c != '-' && c != '.' && c != '/' && c != ':';
  });
}

std::string QuoteCommandArg(std::string_view argument) {
  if (!CommandArgNeedsQuoting(argument)) {
    return std::string(argument);
  }

  std::string quoted;
  quoted.reserve(argument.size() + 2);
  quoted.push_back('\'');
  for (char c : argument) {
    if (c == '\'') {
      quoted += "'\\''";
      continue;
    }
    quoted.push_back(c);
  }
  quoted.push_back('\'');
  return quoted;
}

std::string FormatCommandCompletionToken(const CommandCompletionCandidate& candidate) {
  std::string formatted = QuoteCommandArg(candidate.value);
  if (candidate.append_space) {
    formatted.push_back(' ');
  }
  return formatted;
}

std::vector<CommandCompletionCandidate> CompleteFromValues(std::string_view prefix,
                                                           const std::vector<std::string>& values,
                                                           bool append_space) {
  std::vector<CommandCompletionCandidate> matches;
  for (const std::string& value : values) {
    if (!StartsWith(value, prefix)) {
      continue;
    }
    matches.push_back(CommandCompletionCandidate{value, append_space});
  }
  return matches;
}

std::vector<CommandCompletionCandidate> CompletePath(const std::filesystem::path& project_root,
                                                     std::string_view token,
                                                     bool directories_only) {
  std::vector<CommandCompletionCandidate> matches;
  const std::string token_string(token);
  const bool absolute = !token_string.empty() && token_string.front() == '/';
  const bool ends_with_separator =
      !token_string.empty() && (token_string.back() == '/' || token_string.back() == '\\');

  std::filesystem::path typed_path(token_string);
  std::filesystem::path typed_base = ends_with_separator ? typed_path : typed_path.parent_path();
  const std::string leaf_prefix =
      ends_with_separator ? std::string{} : typed_path.filename().string();
  std::filesystem::path search_directory =
      absolute ? (typed_base.empty() ? std::filesystem::path("/") : typed_base)
               : (typed_base.empty() ? project_root : project_root / typed_base);

  std::error_code error;
  std::filesystem::directory_iterator iterator(search_directory, error);
  if (error) {
    return matches;
  }

  // Advance with the non-throwing increment(error): range-for uses the throwing
  // operator++, so a directory that vanishes, hits an I/O error, or exposes a hostile
  // entry mid-completion would throw std::filesystem_error out of this UI-thread path.
  // A mid-iteration error returns the candidates collected so far.
  const std::filesystem::directory_iterator end;
  for (; !error && iterator != end; iterator.increment(error)) {
    const std::filesystem::directory_entry& entry = *iterator;
    const std::string name = entry.path().filename().string();
    if (!StartsWith(name, leaf_prefix)) {
      continue;
    }

    std::error_code type_error;
    const bool is_directory = entry.is_directory(type_error);
    if (type_error || (directories_only && !is_directory)) {
      continue;
    }

    std::filesystem::path completed_path = typed_base / name;
    if (absolute) {
      completed_path = (typed_base.empty() ? std::filesystem::path("/") : typed_base) / name;
    }

    std::string value = completed_path.lexically_normal().generic_string();
    if (is_directory) {
      if (value.empty()) {
        value = name;
      }
      if (!value.empty() && value.back() != '/') {
        value.push_back('/');
      }
    }

    matches.push_back(CommandCompletionCandidate{
        .value = std::move(value),
        .append_space = !is_directory,
    });
    // Bound candidate collection: completing `/usr/` or a generated directory with
    // hundreds of thousands of entries would otherwise build and sort a huge list
    // on the UI thread. The menu only shows a handful; this cap is well past that.
    constexpr std::size_t kMaxPathCandidates = 2000;
    if (matches.size() >= kMaxPathCandidates) {
      break;
    }
  }

  std::sort(matches.begin(), matches.end(), [](const auto& lhs, const auto& rhs) {
    return lhs.value < rhs.value;
  });
  return matches;
}

std::string JoinCommandArguments(const std::vector<std::string>& args, std::size_t start_index) {
  if (start_index >= args.size()) {
    return {};
  }

  std::string joined;
  for (std::size_t i = start_index; i < args.size(); ++i) {
    if (!joined.empty()) {
      joined.push_back(' ');
    }
    joined += args[i];
  }
  return joined;
}

}  // namespace microide::workspace
