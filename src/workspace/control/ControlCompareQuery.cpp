#include "workspace/control/ControlCompareQuery.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "compare/CompareModel.h"
#include "util/StringUtil.h"
#include "workspace/state/WorkspaceTabState.h"

namespace microide::workspace {
namespace {

const char* RowKindName(compare::CompareRowKind kind) {
  switch (kind) {
    case compare::CompareRowKind::Added:
      return "added";
    case compare::CompareRowKind::Deleted:
      return "deleted";
    case compare::CompareRowKind::Modified:
      return "modified";
    case compare::CompareRowKind::Unchanged:
      break;
  }
  return "unchanged";
}

// A side's exact bytes as hex, for a line that is not valid UTF-8: JSON can only
// carry such a line with U+FFFD in place of each bad run, which moves every byte
// offset after it -- and the spans are byte offsets into the real line. The
// structural oracle read them against the lossy text and reported spans
// "splitting a character" that were on boundaries (soak 1b3aae58e0b7).
std::string HexBytes(std::string_view text) {
  static constexpr char kDigits[] = "0123456789abcdef";
  std::string out;
  out.reserve(text.size() * 2);
  for (const char c : text) {
    const auto byte = static_cast<unsigned char>(c);
    out.push_back(kDigits[byte >> 4]);
    out.push_back(kDigits[byte & 0x0F]);
  }
  return out;
}

util::JsonValue SpansJson(const std::vector<compare::CompareTextSpan>& spans) {
  util::JsonArray out;
  out.reserve(spans.size());
  for (const compare::CompareTextSpan& span : spans) {
    out.push_back(util::JsonValue(util::JsonArray{util::JsonValue(static_cast<std::int64_t>(span.start)),
                                                  util::JsonValue(static_cast<std::int64_t>(span.end))}));
  }
  return util::JsonValue(std::move(out));
}

}  // namespace

util::JsonValue BuildControlCompareJson(const CompareTabState* compare,
                                        const ControlCompareQueryOptions& options) {
  util::JsonObject result;
  result["available"] = util::JsonValue(compare != nullptr);
  if (compare == nullptr) {
    return util::JsonValue(std::move(result));
  }
  const compare::CompareModel& model = compare->model;
  result["leftPath"] = util::JsonValue(compare->left_path.generic_string());
  result["rightPath"] = util::JsonValue(compare->right_path.generic_string());
  result["hunks"] = util::JsonValue(static_cast<std::int64_t>(model.hunks.size()));
  util::JsonArray rows;
  const std::size_t count = std::min(model.rows.size(), options.max_rows);
  rows.reserve(count);
  for (std::size_t i = 0; i < count; ++i) {
    const compare::CompareRow& row = model.rows[i];
    util::JsonObject object;
    object["kind"] = util::JsonValue(std::string(RowKindName(row.kind)));
    object["hunk"] = util::JsonValue(static_cast<std::int64_t>(row.hunk));
    object["leftLine"] = util::JsonValue(static_cast<std::int64_t>(row.left_line));
    object["rightLine"] = util::JsonValue(static_cast<std::int64_t>(row.right_line));
    const bool truncated =
        row.left_text.size() > options.max_text_bytes || row.right_text.size() > options.max_text_bytes;
    object["leftText"] = util::JsonValue(std::string(row.left_text.substr(0, options.max_text_bytes)));
    object["rightText"] = util::JsonValue(std::string(row.right_text.substr(0, options.max_text_bytes)));
    if (truncated) {
      object["textTruncated"] = util::JsonValue(true);
    }
    for (const auto& [key, text] : {std::pair{"leftHex", row.left_text}, std::pair{"rightHex", row.right_text}}) {
      const std::string_view kept = text.substr(0, options.max_text_bytes);
      if (!util::IsValidUtf8(kept)) {
        object[key] = util::JsonValue(HexBytes(kept));
      }
    }
    object["leftSpans"] = SpansJson(row.left_changed_spans);
    object["rightSpans"] = SpansJson(row.right_changed_spans);
    rows.push_back(util::JsonValue(std::move(object)));
  }
  result["rows"] = util::JsonValue(std::move(rows));
  result["rowsTruncated"] = util::JsonValue(count < model.rows.size());
  return util::JsonValue(std::move(result));
}

}  // namespace microide::workspace
