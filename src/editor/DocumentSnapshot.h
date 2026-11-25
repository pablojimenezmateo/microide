#pragma once

#include <cstddef>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace microide::editor {

class DocumentSnapshot {
 public:
  DocumentSnapshot() = default;

  static DocumentSnapshot FromText(std::string text, std::size_t line_count);
  static DocumentSnapshot FromLines(const std::vector<std::string>& lines);

  static DocumentSnapshot FromSpans(std::vector<std::string_view> spans,
                                    std::vector<std::shared_ptr<const void>> owners,
                                    std::size_t line_count);

  std::size_t LineCount() const { return impl_ != nullptr ? impl_->line_count : 0; }
  std::size_t ByteSize() const { return impl_ != nullptr ? impl_->bytes : 0; }
  bool empty() const { return LineCount() == 0; }

  template <typename Fn>
  void ForEachLine(Fn&& fn) const;

  void AppendText(std::string& out) const;
  std::vector<std::string> ToLines() const;

 private:
  struct Impl {
    std::vector<std::string_view> spans;
    std::vector<std::shared_ptr<const void>> owners;
    std::size_t line_count = 0;
    std::size_t bytes = 0;
  };
  std::shared_ptr<const Impl> impl_;
};

template <typename Fn>
void DocumentSnapshot::ForEachLine(Fn&& fn) const {
  if (impl_ == nullptr || impl_->line_count == 0) {
    return;
  }
  std::string scratch;
  bool spanning = false;
  std::size_t emitted = 0;
  for (std::string_view span : impl_->spans) {
    while (!span.empty()) {
      const std::size_t newline = span.find('\n');
      if (newline == std::string_view::npos) {
        scratch.append(span);
        spanning = true;
        break;
      }
      if (spanning) {
        scratch.append(span.substr(0, newline));
        fn(std::string_view(scratch));
        scratch.clear();
        spanning = false;
      } else {
        fn(span.substr(0, newline));
      }
      ++emitted;
      span.remove_prefix(newline + 1);
    }
  }
  if (emitted < impl_->line_count) {
    fn(std::string_view(scratch));
  }
}

}
