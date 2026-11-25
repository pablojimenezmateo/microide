#pragma once

#include <filesystem>
#include <string_view>

namespace microide::util {

struct JsonValue;

class DebugTrace {
 public:
  static bool Enabled();

  static void EnableToFile(const std::filesystem::path& path);

  static void Message(const char* direction, const JsonValue& msg);

  static void Note(const char* category, std::string_view detail);
  static void Note(const char* category, std::string_view detail, std::string_view a);
  static void Note(const char* category, std::string_view detail, long long n);
  static void Note(const char* category, std::string_view detail, std::string_view a, long long n);
  static void Note(const char* category, std::string_view detail, long long n1, long long n2);
};

}
