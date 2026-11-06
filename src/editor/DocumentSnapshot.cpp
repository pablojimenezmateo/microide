#include "editor/DocumentSnapshot.h"

namespace microide::editor {

DocumentSnapshot DocumentSnapshot::FromText(std::string text, std::size_t line_count) {
  if (line_count == 0) {
    return {};
  }
  auto owned = std::make_shared<const std::string>(std::move(text));
  std::vector<std::string_view> spans;
  if (!owned->empty()) {
    spans.emplace_back(*owned);
  }
  return FromSpans(std::move(spans), {std::move(owned)}, line_count);
}

DocumentSnapshot DocumentSnapshot::FromLines(const std::vector<std::string>& lines) {
  std::size_t bytes = lines.empty() ? 0 : lines.size() - 1;
  for (const std::string& line : lines) {
    bytes += line.size();
  }
  std::string text;
  text.reserve(bytes);
  for (std::size_t i = 0; i < lines.size(); ++i) {
    if (i != 0) {
      text.push_back('\n');
    }
    text.append(lines[i]);
  }
  return FromText(std::move(text), lines.size());
}

DocumentSnapshot DocumentSnapshot::FromSpans(std::vector<std::string_view> spans,
                                             std::vector<std::shared_ptr<const void>> owners,
                                             std::size_t line_count) {
  DocumentSnapshot snapshot;
  if (line_count == 0) {
    return snapshot;
  }
  auto impl = std::make_shared<Impl>();
  for (const std::string_view span : spans) {
    impl->bytes += span.size();
  }
  impl->spans = std::move(spans);
  impl->owners = std::move(owners);
  impl->line_count = line_count;
  snapshot.impl_ = std::move(impl);
  return snapshot;
}

void DocumentSnapshot::AppendText(std::string& out) const {
  if (impl_ == nullptr) {
    return;
  }
  out.reserve(out.size() + impl_->bytes);
  for (const std::string_view span : impl_->spans) {
    out.append(span);
  }
}

std::vector<std::string> DocumentSnapshot::ToLines() const {
  std::vector<std::string> lines;
  lines.reserve(LineCount());
  ForEachLine([&lines](std::string_view line) { lines.emplace_back(line); });
  return lines;
}

}  // namespace microide::editor
