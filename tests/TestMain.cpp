#include "compare/CompareModel.h"
#include "project/GitCompareService.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

using microide::compare::BuildCompareModel;
using microide::compare::CompareModel;
using microide::compare::CompareRowKind;
using microide::compare::CompareTextSpan;
using microide::project::CollectGitFileHistory;
using microide::project::ReadGitFileAtCommit;

std::filesystem::path TestRoot() {
  return std::filesystem::path(MICROIDE_TEST_SOURCE_DIR);
}

std::filesystem::path FixturePath(std::string_view relative_path) {
  return TestRoot() / "fixtures" / relative_path;
}

std::string ReadFile(const std::filesystem::path& path) {
  std::ifstream stream(path, std::ios::binary);
  if (!stream) {
    throw std::runtime_error("failed to read file: " + path.string());
  }
  std::ostringstream buffer;
  buffer << stream.rdbuf();
  return buffer.str();
}

void WriteFile(const std::filesystem::path& path, const std::string& content) {
  std::filesystem::create_directories(path.parent_path());
  std::ofstream stream(path, std::ios::binary);
  if (!stream) {
    throw std::runtime_error("failed to write file: " + path.string());
  }
  stream << content;
}

void CopyTree(const std::filesystem::path& source, const std::filesystem::path& destination) {
  std::filesystem::create_directories(destination);
  for (const auto& entry : std::filesystem::recursive_directory_iterator(source)) {
    const auto relative = std::filesystem::relative(entry.path(), source);
    const auto target = destination / relative;
    if (entry.is_directory()) {
      std::filesystem::create_directories(target);
      continue;
    }
    std::filesystem::create_directories(target.parent_path());
    std::filesystem::copy_file(entry.path(), target, std::filesystem::copy_options::overwrite_existing);
  }
}

std::string ShellEscape(std::string_view text) {
  std::string escaped;
  escaped.reserve(text.size() + 8);
  for (char c : text) {
    if (c == '\'') {
      escaped += "'\\''";
    } else {
      escaped.push_back(c);
    }
  }
  return escaped;
}

int RunCommand(const std::string& command) {
  return std::system(command.c_str());
}

void RequireCommandSuccess(const std::string& command, std::string_view context) {
  if (RunCommand(command) != 0) {
    throw std::runtime_error(std::string(context) + ": command failed: " + command);
  }
}

struct CompareSummary {
  int unchanged = 0;
  int added = 0;
  int deleted = 0;
  int modified = 0;
};

CompareSummary Summarize(const CompareModel& model) {
  CompareSummary summary;
  for (const auto& row : model.rows) {
    switch (row.kind) {
      case CompareRowKind::Unchanged:
        ++summary.unchanged;
        break;
      case CompareRowKind::Added:
        ++summary.added;
        break;
      case CompareRowKind::Deleted:
        ++summary.deleted;
        break;
      case CompareRowKind::Modified:
        ++summary.modified;
        break;
    }
  }
  return summary;
}

void Expect(bool condition, std::string_view message) {
  if (!condition) {
    throw std::runtime_error(std::string(message));
  }
}

bool IsUtf8Boundary(std::string_view text, std::size_t offset) {
  if (offset > text.size()) {
    return false;
  }
  if (offset == 0 || offset == text.size()) {
    return true;
  }
  const unsigned char byte = static_cast<unsigned char>(text[offset]);
  return (byte & 0xC0u) != 0x80u;
}

std::string SpanText(std::string_view text, const CompareTextSpan& span) {
  return std::string(text.substr(span.start, span.end - span.start));
}

void TestCompareSimpleFixture() {
  const auto left = ReadFile(FixturePath("diff/simple/left.txt"));
  const auto right = ReadFile(FixturePath("diff/simple/right.txt"));
  const auto model = BuildCompareModel(left, right);
  const auto summary = Summarize(model);

  Expect(model.hunks.size() == 4, "simple diff should produce 4 hunks");
  Expect(model.rows.size() == 10, "simple diff should produce 10 rows");
  Expect(summary.unchanged == 4, "simple diff should produce 4 unchanged rows");
  Expect(summary.modified == 3, "simple diff should produce 3 modified rows");
  Expect(summary.deleted == 1, "simple diff should produce 1 deleted row");
  Expect(summary.added == 2, "simple diff should produce 2 added rows");

  Expect(model.rows[1].kind == CompareRowKind::Modified, "row 2 should be modified");
  Expect(model.rows[1].left_text == "anchor-B-old", "row 2 left text mismatch");
  Expect(model.rows[1].right_text == "anchor-B-new", "row 2 right text mismatch");
  Expect(model.rows[3].hunk == 1, "fourth row should belong to second hunk");
  Expect(model.rows[7].kind == CompareRowKind::Deleted, "row 8 should be deleted");
  Expect(model.rows[9].kind == CompareRowKind::Added, "last row should be added");
  Expect(model.rows[9].right_text == "anchor-H-add-only", "last row text mismatch");
}

void TestCompareCodeFixture() {
  const auto before = ReadFile(FixturePath("diff/code/before.cpp"));
  const auto after = ReadFile(FixturePath("diff/code/after.cpp"));
  const auto model = BuildCompareModel(before, after);
  const auto summary = Summarize(model);

  Expect(model.hunks.size() == 4, "code diff should produce 4 hunks");
  Expect(model.rows.size() == 35, "code diff should produce 35 rows");
  Expect(summary.unchanged == 29, "code diff should produce 29 unchanged rows");
  Expect(summary.modified == 5, "code diff should produce 5 modified rows");
  Expect(summary.deleted == 0, "code diff should produce 0 deleted rows");
  Expect(summary.added == 1, "code diff should produce 1 added row");

  bool saw_pinned = false;
  bool saw_staged = false;
  bool saw_summary_extension = false;
  for (const auto& row : model.rows) {
    if (row.right_text.find("bool pinned = false;") != std::string::npos) {
      saw_pinned = true;
    }
    if (row.right_text.find("{\"staged\", 2, true},") != std::string::npos) {
      saw_staged = true;
    }
    if (row.right_text.find("entries.front().label") != std::string::npos) {
      saw_summary_extension = true;
    }
  }
  Expect(saw_pinned, "code diff should include pinned field row");
  Expect(saw_staged, "code diff should include staged entry row");
  Expect(saw_summary_extension, "code diff should include summary extension row");
}

void TestCompareAsciiChangedSpans() {
  const auto model = BuildCompareModel("alpha uvw omega\n", "alpha xyz omega\n");
  Expect(model.rows.size() == 1, "ascii span diff should produce one row");
  const auto& row = model.rows.front();
  Expect(row.kind == CompareRowKind::Modified, "ascii span row should be modified");
  Expect(row.left_changed_spans.size() == 1, "ascii left should have one changed span");
  Expect(row.right_changed_spans.size() == 1, "ascii right should have one changed span");
  Expect(SpanText(row.left_text, row.left_changed_spans.front()) == "uvw",
         "ascii left changed span should isolate uvw");
  Expect(SpanText(row.right_text, row.right_changed_spans.front()) == "xyz",
         "ascii right changed span should isolate xyz");
}

void TestCompareUtf8ChangedSpans() {
  const auto model = BuildCompareModel("a😀x\n", "a😃x\n");
  Expect(model.rows.size() == 1, "utf8 span diff should produce one row");
  const auto& row = model.rows.front();
  Expect(row.kind == CompareRowKind::Modified, "utf8 span row should be modified");
  Expect(row.left_changed_spans.size() == 1, "utf8 left should have one changed span");
  Expect(row.right_changed_spans.size() == 1, "utf8 right should have one changed span");

  const auto& left_span = row.left_changed_spans.front();
  const auto& right_span = row.right_changed_spans.front();
  Expect(IsUtf8Boundary(row.left_text, left_span.start) && IsUtf8Boundary(row.left_text, left_span.end),
         "utf8 left span should stay on codepoint boundaries");
  Expect(IsUtf8Boundary(row.right_text, right_span.start) &&
             IsUtf8Boundary(row.right_text, right_span.end),
         "utf8 right span should stay on codepoint boundaries");
  Expect(SpanText(row.left_text, left_span) == "😀",
         "utf8 left changed span should isolate the original emoji");
  Expect(SpanText(row.right_text, right_span) == "😃",
         "utf8 right changed span should isolate the replacement emoji");
}

void TestCompareContextAwareAlignment() {
  const auto model = BuildCompareModel(
      R"LEFT(for (const mv of metricValues) {
  try {
    const sourceCurrency =
      mv.currency ?? (await currencyHelper.getOrgCurrency())

    const sourceValue = mv.original_value ?? mv.value

    const conversion = await currencyHelper.convert(
      sourceValue,
      sourceCurrency,
      targetCurrency,
      mv.accrual_date
    )
    if (conversion) {
      conversions.push({
        metric_value_id: mv.id,
        currency: targetCurrency,
        value: conversion.result,
        exchange_rate: conversion.info.rate,
        converted_at: conversion.date
      })
    }
  } catch {
    // Skip failed conversions (caller logs)
  }
})LEFT",
      R"RIGHT(for (const mv of metricValues) {
  try {
    const sourceCurrency =
      mv.currency ?? (orgCurrencyPromise ??= currencyHelper.getOrgCurrency())

    const sourceValue = mv.original_value ?? mv.value
    const exchangeRateDate = mv.converted_at ?? mv.accrual_date

    const conversion = await currencyHelper.convert(
      sourceValue,
      sourceCurrency,
      targetCurrency,
      exchangeRateDate
    )

    if (!conversion) continue

    conversions.push({
      metric_value_id: mv.id,
      currency: targetCurrency,
      value: conversion.result,
      exchange_rate: conversion.info.rate,
      converted_at: conversion.date
    })
  } catch (error) {
    logger.warn(`Failed to convert metric_value ${mv.id}`, {
      metricValueId: mv.id,
      targetCurrency,
    })
  }
})RIGHT");

  bool saw_added_exchange_rate_date = false;
  bool saw_modified_if_guard = false;
  bool saw_paired_push_line = false;
  bool saw_deleted_closing_if = false;
  bool saw_modified_catch = false;
  bool saw_added_logger_warn = false;

  for (const auto& row : model.rows) {
    if (row.kind == CompareRowKind::Added &&
        row.right_text == "    const exchangeRateDate = mv.converted_at ?? mv.accrual_date") {
      saw_added_exchange_rate_date = true;
    }
    if (row.kind == CompareRowKind::Modified && row.left_text == "    if (conversion) {" &&
        row.right_text == "    if (!conversion) continue") {
      saw_modified_if_guard = true;
    }
    if (row.kind == CompareRowKind::Modified &&
        row.left_text == "      conversions.push({" &&
        row.right_text == "    conversions.push({") {
      saw_paired_push_line = true;
    }
    if (row.kind == CompareRowKind::Deleted && row.left_text == "    }") {
      saw_deleted_closing_if = true;
    }
    if (row.kind == CompareRowKind::Modified && row.left_text == "  } catch {" &&
        row.right_text == "  } catch (error) {") {
      saw_modified_catch = true;
    }
    if (row.kind == CompareRowKind::Added &&
        row.right_text.find("logger.warn(`Failed to convert metric_value ${mv.id}`") !=
            std::string::npos) {
      saw_added_logger_warn = true;
    }

    Expect(!(row.left_text == "      conversions.push({" &&
             row.right_text == "    if (!conversion) continue"),
           "context-aware alignment should not pair conversions.push with the continue guard");
    Expect(!(row.left_text == "  } catch {" && row.right_text == "    })"),
           "context-aware alignment should keep catch paired with catch");
  }

  Expect(saw_added_exchange_rate_date, "context-aware diff should keep the new date variable as added");
  Expect(saw_modified_if_guard, "context-aware diff should pair the if guard lines");
  Expect(saw_paired_push_line, "context-aware diff should keep the push call paired after inserted lines");
  Expect(saw_deleted_closing_if, "context-aware diff should keep the removed closing brace as deleted");
  Expect(saw_modified_catch, "context-aware diff should pair catch lines together");
  Expect(saw_added_logger_warn, "context-aware diff should keep logger.warn as added");
}

class TemporaryDirectory {
 public:
  TemporaryDirectory() {
    path_ = std::filesystem::temp_directory_path() /
            ("microide-tests-" + std::to_string(std::rand()) + "-" + std::to_string(std::rand()));
    std::filesystem::create_directories(path_);
  }

  ~TemporaryDirectory() {
    std::error_code error;
    std::filesystem::remove_all(path_, error);
  }

  const std::filesystem::path& path() const { return path_; }

 private:
  std::filesystem::path path_;
};

void TestGitCompareFixture() {
  TemporaryDirectory temp_dir;
  const auto repo_path = temp_dir.path() / "repo";
  const auto base_dir = FixturePath("diff/git/base");
  const auto head_dir = FixturePath("diff/git/head");
  CopyTree(base_dir, repo_path);

  const std::string escaped_repo = ShellEscape(repo_path.string());
  RequireCommandSuccess("git -c init.defaultBranch=main init '" + escaped_repo + "' >/dev/null 2>/dev/null",
                        "git init");
  RequireCommandSuccess("git -C '" + escaped_repo + "' config user.name 'Microide Tests' >/dev/null 2>/dev/null",
                        "git config user.name");
  RequireCommandSuccess(
      "git -C '" + escaped_repo + "' config user.email 'microide-tests@example.com' >/dev/null 2>/dev/null",
                        "git config user.email");
  RequireCommandSuccess("git -C '" + escaped_repo + "' add . >/dev/null 2>/dev/null", "git add base");
  RequireCommandSuccess("git -C '" + escaped_repo + "' commit -m 'base fixture' >/dev/null 2>/dev/null",
                        "git commit base");

  const auto tracked_file = repo_path / "src/session.cpp";
  const auto new_file = repo_path / "src/new_panel.cpp";
  WriteFile(tracked_file, ReadFile(head_dir / "src/session.cpp"));
  WriteFile(new_file, ReadFile(head_dir / "src/new_panel.cpp"));
  RequireCommandSuccess("git -C '" + escaped_repo + "' add . >/dev/null 2>/dev/null", "git add head");
  RequireCommandSuccess("git -C '" + escaped_repo + "' commit -m 'head fixture' >/dev/null 2>/dev/null",
                        "git commit head");

  const auto history = CollectGitFileHistory(repo_path, tracked_file);
  Expect(history.size() == 2, "tracked file should have two commits in history");
  Expect(history[0].subject == "head fixture", "newest history entry subject mismatch");
  Expect(history[1].subject == "base fixture", "oldest history entry subject mismatch");

  const auto latest = ReadGitFileAtCommit(repo_path, tracked_file, history[0].hash);
  const auto original = ReadGitFileAtCommit(repo_path, tracked_file, history[1].hash);
  Expect(latest.has_value(), "latest commit read should succeed");
  Expect(original.has_value(), "base commit read should succeed");
  Expect(latest->exists, "latest tracked file should exist");
  Expect(original->exists, "base tracked file should exist");
  Expect(latest->content == ReadFile(head_dir / "src/session.cpp"), "latest content mismatch");
  Expect(original->content == ReadFile(base_dir / "src/session.cpp"), "base content mismatch");

  const auto missing_in_base = ReadGitFileAtCommit(repo_path, new_file, history[1].hash);
  Expect(missing_in_base.has_value(), "missing file at base commit should produce result");
  Expect(!missing_in_base->exists, "new file should not exist in base commit");
  Expect(missing_in_base->content.empty(), "missing file should have empty content");

  const auto compare_model =
      BuildCompareModel(ReadFile(base_dir / "src/session.cpp"), ReadFile(head_dir / "src/session.cpp"));
  const auto compare_summary = Summarize(compare_model);
  Expect(compare_model.hunks.size() == 3, "git session fixture should produce 3 hunks");
  Expect(compare_model.rows.size() == 26, "git session fixture should produce 26 rows");
  Expect(compare_summary.unchanged == 18, "git session fixture should produce 18 unchanged rows");
  Expect(compare_summary.modified == 2, "git session fixture should produce 2 modified rows");
  Expect(compare_summary.added == 6, "git session fixture should produce 6 added rows");
  Expect(compare_summary.deleted == 0, "git session fixture should produce 0 deleted rows");
}

}  // namespace

int main() {
  try {
    TestCompareSimpleFixture();
    TestCompareCodeFixture();
    TestCompareAsciiChangedSpans();
    TestCompareUtf8ChangedSpans();
    TestCompareContextAwareAlignment();
    TestGitCompareFixture();
  } catch (const std::exception& error) {
    std::cerr << "microide_tests failed: " << error.what() << '\n';
    return 1;
  }
  return 0;
}
