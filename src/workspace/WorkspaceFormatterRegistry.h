#pragma once

#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace microide::workspace {

// Formatter: declarative formatter via subprocess (e.g., clang-format, rustfmt).
struct FormatterSpec {
  std::string id;
  std::string language_id;
  std::string label;
  std::vector<std::string> command;  // e.g., ["clang-format"]
  std::string plugin_id;
};

// Registry for formatters. Formatters run on-save if enabled.
class FormatterRegistry {
 public:
  FormatterRegistry();
  ~FormatterRegistry();

  void Register(const FormatterSpec& spec);
  const std::vector<FormatterSpec>& Specs() const { return specs_; }

  // Find formatter for language_id (returns first if multiple).
  const FormatterSpec* FindFormatter(const std::string& language_id) const;

 private:
  std::vector<FormatterSpec> specs_;
};

}  // namespace microide::workspace
