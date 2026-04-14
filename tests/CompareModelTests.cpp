#include "TestSupport.h"

#include "compare/CompareModel.h"

#include <algorithm>
#include <string>
#include <string_view>

namespace microide::tests {
namespace {

using microide::compare::BuildCompareModel;
using microide::compare::CompareModel;
using microide::compare::CompareRowKind;
using microide::compare::CompareTextSpan;

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
  Expect(model.rows.size() == 11, "simple diff should preserve the terminal empty row");
  Expect(summary.unchanged == 5, "simple diff should keep unchanged rows plus the terminal empty row");
  Expect(summary.modified == 3, "simple diff should produce 3 modified rows");
  Expect(summary.deleted == 1, "simple diff should produce 1 deleted row");
  Expect(summary.added == 2, "simple diff should produce 2 added rows");

  Expect(model.rows[1].kind == CompareRowKind::Modified, "row 2 should be modified");
  Expect(model.rows[1].left_text == "anchor-B-old", "row 2 left text mismatch");
  Expect(model.rows[1].right_text == "anchor-B-new", "row 2 right text mismatch");
  Expect(model.rows[3].hunk == 1, "fourth row should belong to second hunk");
  Expect(model.rows[7].kind == CompareRowKind::Deleted, "row 8 should be deleted");
  Expect(model.rows[9].kind == CompareRowKind::Added, "row 10 should be added");
  Expect(model.rows[9].right_text == "anchor-H-add-only", "added row text mismatch");
  Expect(model.rows[10].kind == CompareRowKind::Unchanged && model.rows[10].left_text.empty() &&
             model.rows[10].right_text.empty(),
         "simple diff should preserve the shared trailing empty line");
}

void TestCompareCodeFixture() {
  const auto before = ReadFile(FixturePath("diff/code/before.cpp"));
  const auto after = ReadFile(FixturePath("diff/code/after.cpp"));
  const auto model = BuildCompareModel(before, after);
  const auto summary = Summarize(model);

  Expect(model.hunks.size() == 4, "code diff should produce 4 hunks");
  Expect(model.rows.size() == 36, "code diff should preserve the terminal empty row");
  Expect(summary.unchanged == 30, "code diff should keep unchanged rows plus the terminal empty row");
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
  const auto model = BuildCompareModel("alpha uvw omega", "alpha xyz omega");
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
  const auto model = BuildCompareModel("a😀x", "a😃x");
  Expect(model.rows.size() == 1, "utf8 span diff should produce one row");
  const auto& row = model.rows.front();
  Expect(row.kind == CompareRowKind::Modified, "utf8 span row should be modified");
  Expect(row.left_changed_spans.size() == 1, "utf8 left should have one changed span");
  Expect(row.right_changed_spans.size() == 1, "utf8 right should have one changed span");

  const auto& left_span = row.left_changed_spans.front();
  const auto& right_span = row.right_changed_spans.front();
  Expect(IsUtf8Boundary(row.left_text, left_span.start) &&
             IsUtf8Boundary(row.left_text, left_span.end),
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
    if (row.kind == CompareRowKind::Modified && row.left_text == "      conversions.push({" &&
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

  Expect(saw_added_exchange_rate_date,
         "context-aware diff should keep the new date variable as added");
  Expect(saw_modified_if_guard, "context-aware diff should pair the if guard lines");
  Expect(saw_paired_push_line,
         "context-aware diff should keep the push call paired after inserted lines");
  Expect(saw_deleted_closing_if,
         "context-aware diff should keep the removed closing brace as deleted");
  Expect(saw_modified_catch, "context-aware diff should pair catch lines together");
  Expect(saw_added_logger_warn, "context-aware diff should keep logger.warn as added");
}

void TestCompareLargeInputsUseBoundedFallback() {
  std::string left = "header\n";
  std::string right = "header\n";
  for (int i = 0; i < 1500; ++i) {
    left += "left-" + std::to_string(i) + '\n';
    right += "right-" + std::to_string(i) + '\n';
  }
  left += "footer\n";
  right += "footer\n";

  const auto model = BuildCompareModel(left, right);
  const auto summary = Summarize(model);

  Expect(model.rows.size() == 1503,
         "large fallback compare should preserve row cardinality including the terminal empty row");
  Expect(model.hunks.size() == 1, "large fallback compare should produce one changed hunk");
  Expect(summary.unchanged == 3,
         "large fallback compare should preserve shared prefix, suffix, and terminal empty row");
  Expect(summary.modified == 1500, "large fallback compare should pair middle rows as modified");
  Expect(model.rows.front().kind == CompareRowKind::Unchanged &&
             model.rows.front().left_text == "header",
         "large fallback compare should keep the shared prefix unchanged");
  Expect(model.rows[model.rows.size() - 2].kind == CompareRowKind::Unchanged &&
             model.rows[model.rows.size() - 2].right_text == "footer",
         "large fallback compare should keep the shared suffix unchanged");
  Expect(model.rows.back().kind == CompareRowKind::Unchanged &&
             model.rows.back().left_text.empty() && model.rows.back().right_text.empty(),
         "large fallback compare should preserve the shared trailing empty row");
}

void TestCompareLargeInputsUseCoarseChangedSpans() {
  std::string left;
  std::string right;
  left.reserve(3000 * 64);
  right.reserve(3000 * 64);
  for (int i = 0; i < 2200; ++i) {
    left += "msgid \"left line " + std::to_string(i) + " alpha beta gamma\"\n";
    right += "msgid \"right line " + std::to_string(i) + " alpha beta gamma\"\n";
  }

  const auto model = BuildCompareModel(left, right);

  Expect(!model.rows.empty(), "coarse-span fixture should produce compare rows");
  const auto it = std::find_if(model.rows.begin(), model.rows.end(), [](const auto& row) {
    return row.kind == CompareRowKind::Modified;
  });
  Expect(it != model.rows.end(), "coarse-span fixture should include modified rows");
  Expect(it->left_changed_spans.size() == 1 && it->right_changed_spans.size() == 1,
         "large compare rows should use one coarse changed span per side");
  Expect(it->left_changed_spans.front().start == 0 &&
             it->left_changed_spans.front().end == it->left_text.size(),
         "large compare coarse left span should cover the full line");
  Expect(it->right_changed_spans.front().start == 0 &&
             it->right_changed_spans.front().end == it->right_text.size(),
         "large compare coarse right span should cover the full line");
}

void TestCompareLargeIdenticalInputsStayUnchanged() {
  std::string text;
  text.reserve(3000 * 24);
  for (int i = 0; i < 3000; ++i) {
    text += "msgid \"key_" + std::to_string(i) + "\"\n";
  }

  const auto model = BuildCompareModel(text, text);
  const auto summary = Summarize(model);
  Expect(model.hunks.empty(), "large identical compare should not create hunks");
  Expect(model.rows.size() == 3001,
         "large identical compare should keep all rows including the terminal empty row");
  Expect(summary.unchanged == 3001 && summary.added == 0 && summary.deleted == 0 &&
             summary.modified == 0,
         "large identical compare should be fully unchanged");
}

}  // namespace

void RegisterCompareModelTests(std::vector<TestCase>& tests) {
  AddTest(tests, "Compare/SimpleFixture", TestCompareSimpleFixture);
  AddTest(tests, "Compare/CodeFixture", TestCompareCodeFixture);
  AddTest(tests, "Compare/AsciiChangedSpans", TestCompareAsciiChangedSpans);
  AddTest(tests, "Compare/Utf8ChangedSpans", TestCompareUtf8ChangedSpans);
  AddTest(tests, "Compare/ContextAwareAlignment", TestCompareContextAwareAlignment);
  AddTest(tests, "Compare/LargeInputsUseBoundedFallback",
          TestCompareLargeInputsUseBoundedFallback);
  AddTest(tests, "Compare/LargeInputsUseCoarseChangedSpans",
          TestCompareLargeInputsUseCoarseChangedSpans);
  AddTest(tests, "Compare/LargeIdenticalInputsStayUnchanged",
          TestCompareLargeIdenticalInputsStayUnchanged);
}

}  // namespace microide::tests
