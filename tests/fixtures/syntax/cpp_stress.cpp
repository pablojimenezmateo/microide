#include <array>
#include <concepts>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#define MICROIDE_TEST_FLAG(name) "flag:" name
#define MICROIDE_JOIN(a, b) a##b

namespace microide::syntax_fixture {

template <typename T>
concept NumberLike = std::integral<T> || std::floating_point<T>;

struct Widget {
  int id = 7;
  std::string name = "widget";
  bool enabled = true;
};

constexpr std::string_view kRaw = R"CPP(
line one
line two with "quotes" and // comment text inside the string
)CPP";

template <NumberLike T>
constexpr T Blend(T left, T right) {
  return left + (right - left) / static_cast<T>(2);
}

std::optional<std::string> FormatWidget(const Widget& widget) {
  // Single-line comment with 1234 and 0xff values.
  if (!widget.enabled) {
    return std::nullopt;
  }

  /*
    Multi-line comment with punctuation, braces {}, and operators ++ --.
  */
  const auto label = [name = widget.name, id = widget.id]() -> std::string {
    return name + ":" + std::to_string(id);
  };

  const double mixed = Blend(1.5, 9.5);
  const char marker = '\n';
  return label() + "|" + std::to_string(mixed) + "|" + marker;
}

}  // namespace microide::syntax_fixture
