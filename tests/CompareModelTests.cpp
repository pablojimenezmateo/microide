#include "TestSupport.h"

#include "CompareSummary.h"

#include "compare/CompareModel.h"
#include "compare/StructuralDiff.h"
#include "util/StringUtil.h"

#include <algorithm>
#include <random>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace microide::tests {
namespace {

using microide::compare::BuildCompareModel;
using microide::compare::BuildCompareModelInto;
using microide::compare::BuildCompareModelProfiled;
using microide::compare::CompareBuildOptions;
using microide::compare::CompareModel;
using microide::compare::CompareRow;
using microide::compare::CompareRowKind;
using microide::compare::CompareSideChange;
using microide::compare::CompareTextSpan;
using microide::compare::kStructuralDiffVertexBudget;
using microide::compare::LeftSideChange;
using microide::compare::RightSideChange;

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

std::string DescribeRows(const CompareModel& model,
                         std::size_t start_index = 0,
                         std::size_t max_rows = 32) {
  std::ostringstream stream;
  const std::size_t end_index = std::min(model.rows.size(), start_index + max_rows);
  for (std::size_t index = start_index; index < end_index; ++index) {
    const auto& row = model.rows[index];
    char kind = '=';
    switch (row.kind) {
      case CompareRowKind::Added:
        kind = '+';
        break;
      case CompareRowKind::Deleted:
        kind = '-';
        break;
      case CompareRowKind::Modified:
        kind = '~';
        break;
      case CompareRowKind::Unchanged:
      default:
        kind = '=';
        break;
    }
    stream << index << ' ' << kind << " L" << row.left_line << " R" << row.right_line << " | "
           << row.left_text << " || " << row.right_text << '\n';
  }
  return stream.str();
}

bool ByteCoveredByChangedSpan(const std::vector<CompareTextSpan>& spans, std::size_t offset) {
  return std::any_of(spans.begin(), spans.end(), [&](const CompareTextSpan& span) {
    return offset >= span.start && offset < span.end;
  });
}

bool RangeIsUnchanged(std::string_view text,
                      const std::vector<CompareTextSpan>& spans,
                      std::string_view needle) {
  const std::size_t start = text.find(needle);
  if (start == std::string_view::npos) {
    return false;
  }
  for (std::size_t offset = start; offset < start + needle.size(); ++offset) {
    if (ByteCoveredByChangedSpan(spans, offset)) {
      return false;
    }
  }
  return true;
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
         "utf8 left changed span should cover the original emoji, got '" +
             std::string(SpanText(row.left_text, left_span)) + "'");
  Expect(SpanText(row.right_text, right_span) == "😃",
         "utf8 right changed span should cover the replacement emoji, got '" +
             std::string(SpanText(row.right_text, right_span)) + "'");
}

void TestCompareCodeTokenChangedSpans() {
  const auto model = BuildCompareModel(
      "item.active ? theme_.text_primary : theme_.text_secondary, background,",
      "item.active ? theme_.chrome_active_text : theme_.chrome_text, background,");
  Expect(model.rows.size() == 1, "code token span diff should produce one row");
  const auto& row = model.rows.front();
  Expect(row.kind == CompareRowKind::Modified, "code token span row should be modified");
  Expect(row.left_changed_spans.size() == 2,
         "code token span diff should isolate the two replaced left identifiers");
  Expect(row.right_changed_spans.size() == 2,
         "code token span diff should isolate the two replaced right identifiers");
  Expect(SpanText(row.left_text, row.left_changed_spans[0]) == "text_primary",
         "code token span diff should isolate left text_primary");
  Expect(SpanText(row.left_text, row.left_changed_spans[1]) == "text_secondary",
         "code token span diff should isolate left text_secondary");
  Expect(SpanText(row.right_text, row.right_changed_spans[0]) == "chrome_active_text",
         "code token span diff should isolate right chrome_active_text");
  Expect(SpanText(row.right_text, row.right_changed_spans[1]) == "chrome_text",
         "code token span diff should isolate right chrome_text");
}

void TestCompareAssignmentPrefixStaysUnchanged() {
  const std::string left =
      "const groupedOrders = (invoice?.orders ?? []).concat(...(invoice?.order_lines.map(cp) => {";
  const std::string right =
      "const groupedOrders = getExpandedInvoiceOrders(invoice).reduce((acc: GroupedOrders[], item: ExpandedInvoiceOrder) => {";
  const auto model = BuildCompareModel(left, right);

  Expect(model.rows.size() == 1, "assignment prefix diff should produce one row");
  const auto& row = model.rows.front();
  Expect(row.kind == CompareRowKind::Modified, "assignment prefix row should be modified");
  Expect(!row.left_changed_spans.empty() && !row.right_changed_spans.empty(),
         "assignment prefix diff should still report changed spans");
  Expect(RangeIsUnchanged(row.left_text, row.left_changed_spans, "const groupedOrders = "),
         "assignment prefix diff should keep the left assignment prefix unchanged");
  Expect(RangeIsUnchanged(row.right_text, row.right_changed_spans, "const groupedOrders = "),
         "assignment prefix diff should keep the right assignment prefix unchanged");
}

void TestCompareLongQueryChangedSpansKeepSharedClause() {
  const std::string left =
      "*, has_pending_orders, profiles!invoices_profile_id_fkey(*, "
      "groups_profiles(group_id)), order_lines(*, orders(${ordersQuery})), "
      "p_total: orders(value.sum()),";
  const std::string right =
      "*, has_pending_orders, profiles!invoices_profile_id_fkey(*, "
      "groups_profiles(group_id)), orders(${ordersQuery}), p_total: orders(value.sum()), "
      "pi_total: order_lines(amount.sum()), issues(*)";
  const auto model = BuildCompareModel(left, right);

  Expect(model.rows.size() == 1, "long query span diff should produce one row");
  const auto& row = model.rows.front();
  Expect(row.kind == CompareRowKind::Modified, "long query span row should be modified");
  Expect(!row.left_changed_spans.empty() && !row.right_changed_spans.empty(),
         "long query span diff should still report changed spans");
  Expect(RangeIsUnchanged(row.left_text, row.left_changed_spans, "p_total: orders(value.sum()),"),
         "long query left should keep the shared aggregate clause unchanged");
  Expect(RangeIsUnchanged(row.right_text, row.right_changed_spans,
                          "p_total: orders(value.sum()),"),
         "long query right should keep the shared aggregate clause unchanged");
}

void TestCompareRepeatedStructureKeepsSharedBlockUnchanged() {
  const auto model = BuildCompareModel(
      R"LEFT(const order = [
  {
    column: 'created_at',
    options: {
      ascending: false,
      referencedTable: 'issues'
    },
  },
  {
    column: 'effective_date',
    options: {
      ascending: true,
      referencedTable: 'orders'
    },
  },
  {
    column: 'id',
    options: {
      ascending: true,
      referencedTable: 'orders'
    },
  },
  {
    column: 'effective_date',
    options: {
      ascending: true,
      referencedTable: 'order_lines.orders'
    },
  },
]
return order
)LEFT",
      R"RIGHT(const order = [
  {
    column: 'created_at',
    options: {
      ascending: false,
      referencedTable: 'issues'
    },
  },
  {
    column: 'effective_date',
    options: {
      ascending: true,
      referencedTable: 'orders'
    },
  },
  {
    column: 'id',
    options: {
      ascending: true,
      referencedTable: 'orders'
    },
  },
]
const { isLoading, items: invoices } = useSupabaseTable<
  InvoicesDetailsType,
  TablesInsert<'invoices'>
>()
return order
)RIGHT");

  bool saw_issues_unchanged = false;
  bool saw_id_unchanged = false;
  bool saw_deleted_installment_reference = false;
  bool saw_added_table_hook = false;

  for (const auto& row : model.rows) {
    if (row.kind == CompareRowKind::Unchanged &&
        row.left_text == "      referencedTable: 'issues'") {
      saw_issues_unchanged = true;
    }
    if (row.kind == CompareRowKind::Unchanged && row.left_text == "    column: 'id',") {
      saw_id_unchanged = true;
    }
    if (row.kind == CompareRowKind::Deleted &&
        row.left_text == "      referencedTable: 'order_lines.orders'") {
      saw_deleted_installment_reference = true;
    }
    if (row.kind == CompareRowKind::Added &&
        row.right_text == "const { isLoading, items: invoices } = useSupabaseTable<") {
      saw_added_table_hook = true;
    }

    Expect(!(row.left_text == "    column: 'id'," &&
             row.right_text == "const { isLoading, items: invoices } = useSupabaseTable<"),
           "repeated structure diff should not pair the shared order block with the new hook");
  }

  Expect(saw_issues_unchanged,
         "repeated structure diff should keep the first shared object entry unchanged");
  Expect(saw_id_unchanged,
         "repeated structure diff should keep the third shared object entry unchanged");
  Expect(saw_deleted_installment_reference,
         "repeated structure diff should keep the extra order_lines entry deleted");
  Expect(saw_added_table_hook,
         "repeated structure diff should keep the new useSupabaseTable block added");
}

void TestCompareImportExpansionKeepsFollowingImportsUnchanged() {
  std::string left =
      "import {\n"
      "  GroupedOrders,\n"
      "  HistoricInvoicesType,\n"
      "  OrderWithJoins,\n"
      "  InvoicesDetailsType\n"
      "} from '@example/business/custom_types'\n"
      "import { Database, Tables, TablesInsert } from '@example/business/db_types'\n"
      "import { ordersQuery } from '@example/business/shared_queries'\n"
      "import { QueryKeys } from '@example/hooks/queryKeys'\n"
      "import { useOrgContext } from '@example/hooks/state/organization'\n"
      "import { useSupabaseClient } from '@example/hooks/supabase'\n"
      "import { useSupabaseTable } from '@example/hooks/supabase-table'\n";
  std::string right =
      "import {\n"
      "  GroupedOrders,\n"
      "  HistoricInvoicesType,\n"
      "  OrderWithJoins,\n"
      "  InvoicesDetailsType\n"
      "} from '@example/business/custom_types'\n"
      "import { Database, Tables, TablesInsert } from '@example/business/db_types'\n"
      "import {\n"
      "  ordersQuery,\n"
      "  ordersQueryWithoutLines\n"
      "} from '@example/business/shared_queries'\n"
      "import { QueryKeys } from '@example/hooks/queryKeys'\n"
      "import { useOrgContext } from '@example/hooks/state/organization'\n"
      "import { useSupabaseClient } from '@example/hooks/supabase'\n"
      "import { useSupabaseTable } from '@example/hooks/supabase-table'\n";

  for (int i = 0; i < 220; ++i) {
    left += "const filler_" + std::to_string(i) + " = " + std::to_string(i) + ";\n";
    right += "const filler_" + std::to_string(i) + " = " + std::to_string(i) + ";\n";
  }

  left +=
      "import { InvoiceNavigator } from './invoice-navigator'\n"
      "import InvoicesHistoricChart from './invoicesHistoricChart'\n"
      "import InvoiceWaterfallChart from './invoiceWaterfallChart'\n";
  right +=
      "import { InvoiceNavigator } from './invoice-navigator'\n"
      "import InvoicesHistoricChart from './invoicesHistoricChart'\n"
      "import InvoiceWaterfallChart from './invoiceWaterfallChart'\n"
      "import { ExpandedInvoiceOrder, getExpandedInvoiceOrders } from './utils'\n";

  for (int i = 220; i < 520; ++i) {
    left += "const filler_" + std::to_string(i) + " = " + std::to_string(i) + ";\n";
    right += "const filler_" + std::to_string(i) + " = " + std::to_string(i) + ";\n";
  }

  left +=
      "    column: 'id',\n"
      "    options: {\n"
      "      ascending: true,\n"
      "      referencedTable: 'order_lines.orders.order_lines'\n"
      "    }\n";
  right +=
      "    column: 'id',\n"
      "    options: {\n"
      "      ascending: true,\n"
      "      referencedTable: 'order_lines'\n"
      "    }\n";

  const auto model = BuildCompareModel(left, right);

  bool saw_query_keys_unchanged = false;
  bool saw_supabase_table_unchanged = false;
  bool saw_shared_query_deleted = false;
  bool saw_shared_query_added = false;
  bool saw_late_reference_modified = false;

  for (const auto& row : model.rows) {
    if (row.kind == CompareRowKind::Unchanged &&
        row.left_text == "import { QueryKeys } from '@example/hooks/queryKeys'") {
      saw_query_keys_unchanged = true;
    }
    if (row.kind == CompareRowKind::Unchanged &&
        row.left_text == "import { useSupabaseTable } from '@example/hooks/supabase-table'") {
      saw_supabase_table_unchanged = true;
    }
    if (row.left_text == "import { ordersQuery } from '@example/business/shared_queries'" &&
        LeftSideChange(row) == CompareSideChange::LayoutOnly) {
      saw_shared_query_deleted = true;
    }
    if (row.kind == CompareRowKind::Added && row.right_text == "  ordersQueryWithoutLines" &&
        row.right_changed_spans.size() == 1 &&
        SpanText(row.right_text, row.right_changed_spans.front()) == "ordersQueryWithoutLines") {
      saw_shared_query_added = true;
    }
    if (row.kind == CompareRowKind::Modified &&
        row.left_text == "      referencedTable: 'order_lines.orders.order_lines'" &&
        row.right_text == "      referencedTable: 'order_lines'") {
      saw_late_reference_modified = true;
    }

    Expect(!(row.left_text == "import { QueryKeys } from '@example/hooks/queryKeys'" &&
             row.right_text == "} from '@example/business/shared_queries'"),
           "import expansion diff should not pair the following unchanged import with the split import tail");
  }

  Expect(saw_query_keys_unchanged,
         "import expansion diff should keep QueryKeys import unchanged after the split import");
  Expect(saw_supabase_table_unchanged,
         "import expansion diff should keep later unchanged imports aligned");
  Expect(saw_shared_query_deleted,
         ("import expansion diff should read the single-line import as a layout change\n" +
          DescribeRows(model, 6, 12))
             .c_str());
  Expect(saw_shared_query_added,
         "import expansion diff should mark only the extra shared query symbol novel");
  Expect(saw_late_reference_modified,
         "import expansion diff should still pair the later referencedTable edit");
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
    if (row.right_text.find("logger.warn(`Failed to convert metric_value ${mv.id}`") !=
            std::string::npos &&
        RightSideChange(row) == CompareSideChange::Novel &&
        SpanText(row.right_text, row.right_changed_spans.front()) == "logger.warn") {
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
  Expect(saw_added_logger_warn, "context-aware diff should mark logger.warn novel");
}

std::vector<std::string> ReconstructSide(const CompareModel& model, bool left) {
  std::vector<std::string> lines;
  for (const auto& row : model.rows) {
    if (left ? (row.left_line != 0) : (row.right_line != 0)) {
      lines.emplace_back(left ? row.left_text : row.right_text);
    }
  }
  return lines;
}

void TestCompareModelPreservesBothSidesRoundTrip() {
  const auto check = [](const std::string& left, const std::string& right, const char* label) {
    const auto model = BuildCompareModel(left, right);
    const auto expected_left = microide::util::SplitLines(left);
    const auto expected_right = microide::util::SplitLines(right);
    Expect(ReconstructSide(model, true) == expected_left,
           (std::string("left side must round-trip exactly: ") + label).c_str());
    Expect(ReconstructSide(model, false) == expected_right,
           (std::string("right side must round-trip exactly: ") + label).c_str());
  };

  check(ReadFile(FixturePath("diff/simple/left.txt")),
        ReadFile(FixturePath("diff/simple/right.txt")), "simple fixture");
  check(ReadFile(FixturePath("diff/code/before.cpp")),
        ReadFile(FixturePath("diff/code/after.cpp")), "code fixture");

  {
    std::string left = "header\n";
    std::string right = "header\n";
    for (int i = 0; i < 1500; ++i) {
      left += "left-" + std::to_string(i) + '\n';
      right += "right-" + std::to_string(i) + '\n';
    }
    left += "footer\n";
    right += "footer\n";
    check(left, right, "large bounded fallback");
  }

  {
    std::string left;
    std::string right;
    for (int i = 0; i < 1200; ++i) {
      const std::string anchor = "ANCHOR_" + std::to_string(i) + "\n";
      left += anchor + "left_only_" + std::to_string(i) + "\n";
      right += anchor + "right_only_" + std::to_string(i) + "\n";
    }
    check(left, right, "interleaved anchors fallback");
  }
}

void TestCompareModelAnchoredFallbackMatchesEveryUniqueAnchor() {
  constexpr int kAnchors = 1200;
  std::string left;
  std::string right;
  for (int i = 0; i < kAnchors; ++i) {
    const std::string anchor = "ANCHOR_" + std::to_string(i) + "\n";
    left += anchor + "left_only_" + std::to_string(i) + "\n";
    right += anchor + "right_only_" + std::to_string(i) + "\n";
  }

  const auto model = BuildCompareModel(left, right);
  std::size_t matched_anchors = 0;
  for (const auto& row : model.rows) {
    if (row.kind != microide::compare::CompareRowKind::Unchanged) {
      continue;
    }
    if (row.left_text.rfind("ANCHOR_", 0) == 0) {
      Expect(row.left_text == row.right_text,
             "an unchanged anchor row must carry the same text on both sides");
      ++matched_anchors;
    }
  }
  Expect(matched_anchors == static_cast<std::size_t>(kAnchors),
         "the anchored fallback must pair every unique anchor line; a scratch buffer "
         "clobbered by a deeper recursion level shows up here as a shortfall");
}

void TestCompareIntralineFallbackCoversChangedRegion() {
  std::string left;
  std::string right;
  for (int i = 0; i < 600; ++i) {
    left += "tokL" + std::to_string(i) + " ";
    right += "tokR" + std::to_string(i) + " ";
  }
  const auto model = BuildCompareModel(left, right);
  Expect(model.rows.size() == 1, "single oversized modified line should produce one row");
  const auto& row = model.rows.front();
  Expect(row.kind == CompareRowKind::Modified, "oversized line row should be modified");
  Expect(!row.left_changed_spans.empty() && !row.right_changed_spans.empty(),
         "intra-line fallback should still report changed spans covering the difference");
  for (const auto& span : row.left_changed_spans) {
    Expect(span.start <= span.end && span.end <= row.left_text.size(),
           "left fallback span must stay within bounds");
    Expect(IsUtf8Boundary(row.left_text, span.start) && IsUtf8Boundary(row.left_text, span.end),
           "left fallback span must stay on codepoint boundaries");
  }
  for (const auto& span : row.right_changed_spans) {
    Expect(span.start <= span.end && span.end <= row.right_text.size(),
           "right fallback span must stay within bounds");
  }
}

void TestCompareReversedUniqueLinesStayBounded() {
  constexpr int kLines = 4000;
  std::string left;
  std::string right;
  for (int i = 0; i < kLines; ++i) {
    left += "u" + std::to_string(i) + '\n';
    right += "u" + std::to_string(kLines - 1 - i) + '\n';
  }

  const auto model = BuildCompareModel(left, right);
  Expect(model.rows.size() >= static_cast<std::size_t>(kLines),
         "reversed-unique compare should cover all lines without overflowing the stack");
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

void TestCompareLargePaddedAssignmentPrefixStaysUnchanged() {
  std::string left;
  std::string right;
  left.reserve(2600 * 48);
  right.reserve(2600 * 48);
  for (int i = 0; i < 1200; ++i) {
    const std::string filler =
        "const filler_" + std::to_string(i) + " = shared_" + std::to_string(i) + ";\n";
    left += filler;
    right += filler;
  }

  left +=
      "const groupedOrders = (invoice?.orders ?? []).concat(...(invoice?.order_lines.map(cp) => {\n";
  right +=
      "const groupedOrders = getExpandedInvoiceOrders(invoice).reduce((acc: GroupedOrders[], item: ExpandedInvoiceOrder) => {\n";

  for (int i = 1200; i < 2600; ++i) {
    const std::string filler =
        "const filler_" + std::to_string(i) + " = shared_" + std::to_string(i) + ";\n";
    left += filler;
    right += filler;
  }

  const auto model = BuildCompareModel(left, right);

  const auto it = std::find_if(model.rows.begin(), model.rows.end(), [](const auto& row) {
    return row.kind == CompareRowKind::Modified &&
           row.left_text.find("const groupedOrders = ") != std::string::npos;
  });
  Expect(it != model.rows.end(),
         "large padded assignment fixture should keep the modified assignment row paired");
  Expect(!it->left_changed_spans.empty() && !it->right_changed_spans.empty(),
         "large padded assignment fixture should still report detailed changed spans");
  Expect(RangeIsUnchanged(it->left_text, it->left_changed_spans, "const groupedOrders = "),
         "large padded assignment fixture should keep the left assignment prefix unchanged");
  Expect(RangeIsUnchanged(it->right_text, it->right_changed_spans, "const groupedOrders = "),
         "large padded assignment fixture should keep the right assignment prefix unchanged");
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

void TestProfiledCompareBuildMatchesUnprofiledModel() {
  const std::string left =
      "alpha\n"
      "beta\n"
      "gamma\n"
      "delta\n";
  const std::string right =
      "alpha\n"
      "beta updated\n"
      "gamma\n"
      "delta extra\n";

  const CompareModel plain = BuildCompareModel(left, right);
  const auto profiled = BuildCompareModelProfiled(left, right);

  Expect(profiled.model.rows.size() == plain.rows.size(),
         "profiled compare build should keep the same row count");
  Expect(profiled.model.hunks.size() == plain.hunks.size(),
         "profiled compare build should keep the same hunk count");
  Expect(profiled.profile.total_ns >= profiled.profile.split_lines_ns +
                                         profiled.profile.line_alignment_ns +
                                         profiled.profile.hunk_refinement_ns,
         "profiled compare build total should include the staged timings");
  Expect(profiled.profile.total_ns > 0,
         "profiled compare build should report a non-zero total duration");

  for (std::size_t index = 0; index < plain.rows.size(); ++index) {
    const auto& expected = plain.rows[index];
    const auto& actual = profiled.model.rows[index];
    Expect(actual.left_text == expected.left_text && actual.right_text == expected.right_text &&
               actual.left_line == expected.left_line && actual.right_line == expected.right_line &&
               actual.kind == expected.kind && actual.hunk == expected.hunk &&
               actual.left_changed_spans.size() == expected.left_changed_spans.size() &&
               actual.right_changed_spans.size() == expected.right_changed_spans.size(),
           ("profiled compare build should preserve row semantics\n" +
            DescribeRows(profiled.model, index, 4))
               .c_str());
  }
}

void TestCompareIgnoreWhitespacePreservesRightText() {
  CompareBuildOptions options;
  options.ignore_whitespace = true;
  const CompareModel model = BuildCompareModel("foo\n  bar", "foo\nbar", options);
  Expect(model.rows.size() == 2, "two unchanged rows under ignore_whitespace");
  Expect(model.rows[1].kind == CompareRowKind::Unchanged,
         "a whitespace-only difference is Unchanged when whitespace is ignored");
  Expect(model.rows[1].left_text == "  bar", "left column keeps the left file's text");
  Expect(model.rows[1].right_text == "bar",
         "right column must reflect the right file, not a copy of the left line");
}

const CompareRow* FindRowByRightText(const CompareModel& model, std::string_view right_text) {
  for (const auto& row : model.rows) {
    if (row.right_text == right_text) {
      return &row;
    }
  }
  return nullptr;
}

void TestCompareIgnoreWhitespaceInteriorHunkLine() {
  const std::string left = "head\nAAA\n    mid\nBBB\ntail\n";
  const std::string right = "head\nXXX\nmid\nYYY\ntail\n";

  CompareBuildOptions ignore_ws;
  ignore_ws.ignore_whitespace = true;
  const CompareModel ignored = BuildCompareModel(left, right, ignore_ws);
  const CompareRow* mid = FindRowByRightText(ignored, "mid");
  Expect(mid != nullptr, "the whitespace-only interior line should produce a single paired row");
  Expect(mid->kind == CompareRowKind::Unchanged,
         "an interior whitespace-only difference must be Unchanged when whitespace is ignored");
  Expect(mid->left_text == "    mid",
         "the left column must keep the left file's indented text");
  Expect(mid->right_text == "mid",
         "the right column must reflect the right file, not a copy of the left line");

  const CompareModel exact = BuildCompareModel(left, right);
  const CompareRow* mid_exact = FindRowByRightText(exact, "mid");
  Expect(mid_exact != nullptr && mid_exact->kind == CompareRowKind::Modified,
         "without ignore_whitespace the interior line stays Modified");
}

void TestCompareLongLineSkipsIntralineRefinement() {
  const std::string middle_left(300000, 'a');
  const std::string middle_right(300000, 'b');
  const std::string left = "PREFIX" + middle_left + "SUFFIX\n";
  const std::string right = "PREFIX" + middle_right + "SUFFIX\n";

  const CompareModel model = BuildCompareModel(left, right);

  const CompareRow* modified = nullptr;
  for (const auto& row : model.rows) {
    if (row.kind == CompareRowKind::Modified) {
      modified = &row;
      break;
    }
  }
  Expect(modified != nullptr, "the long differing line should align as a Modified row");
  Expect(modified->left_changed_spans.size() == 1 &&
             modified->left_changed_spans[0].start == 0 &&
             modified->left_changed_spans[0].end == modified->left_text.size(),
         "an over-long modified line should mark the whole left side changed");
  Expect(modified->right_changed_spans.size() == 1 &&
             modified->right_changed_spans[0].start == 0 &&
             modified->right_changed_spans[0].end == modified->right_text.size(),
         "an over-long modified line should mark the whole right side changed");
}

void TestCompareManyTokenLineBoundsAlignmentDp() {
  std::string left_line;
  std::string right_line;
  left_line.reserve(30000 * 7);
  right_line.reserve(30000 * 7);
  for (int i = 0; i < 30000; ++i) {
    if (i != 0) {
      left_line += ' ';
      right_line += ' ';
    }
    left_line += 'a';
    left_line += std::to_string(i);
    right_line += 'b';
    right_line += std::to_string(i);
  }
  const std::string left = left_line + "\n";
  const std::string right = right_line + "\n";

  const auto result = BuildCompareModelProfiled(left, right);
  Expect(result.profile.graph_vertices == 0,
         "a token-dense line of bare atoms must not reach the graph search");

  const CompareRow* modified = nullptr;
  for (const auto& row : result.model.rows) {
    if (row.kind == CompareRowKind::Modified) {
      modified = &row;
      break;
    }
  }
  Expect(modified != nullptr, "two differing token-dense lines should align as one Modified row");
  Expect(modified->left_text == left_line && modified->right_text == right_line,
         "the bounded build must preserve both sides verbatim");
}

void TestCompareLargeRewrittenHunkStaysBoundedAndNovel() {
  const std::string common_prefix(160, 'p');
  const std::string common_suffix(160, 's');
  constexpr int kLineCount = 400;
  std::string left;
  std::string right;
  for (int i = 0; i < kLineCount; ++i) {
    const std::string idx = std::to_string(i);
    left += common_prefix + "L" + idx + "L" + common_suffix + "\n";
    right += common_prefix + "R" + idx + "R" + common_suffix + "\n";
  }

  const auto result = BuildCompareModelProfiled(left, right);
  Expect(result.profile.graph_vertices <= kStructuralDiffVertexBudget + 64,
         "the structural search must stay within its vertex budget");
  std::size_t modified = 0;
  for (const auto& row : result.model.rows) {
    if (row.kind != CompareRowKind::Modified) {
      continue;
    }
    ++modified;
    Expect(LeftSideChange(row) == CompareSideChange::Novel &&
               RightSideChange(row) == CompareSideChange::Novel,
           "every rewritten line must be novel on both sides");
  }
  Expect(modified == kLineCount, "a line-for-line rewrite must pair every line");
}

}

void TestCompareLineEqualityInterningMatchesPredicate() {
  const auto strip = [](std::string_view line) {
    std::string out;
    for (const char c : line) {
      if (!microide::util::IsAsciiSpace(static_cast<unsigned char>(c))) {
        out.push_back(c);
      }
    }
    return out;
  };

  const std::vector<std::string> alphabet = {
      "alpha", " alpha", "alpha ", "  alpha  ", "a l p h a", "alpha\t", "\talpha",
      "beta",  "beta ",  "",       " ",         "\t",        "  ",      "a",
      "ab",    "a b",    " a b ",  "b a",       "alphabeta", "alpha beta",
  };

  std::uint64_t state = 0x243F6A8885A308D3ull;
  const auto next = [&state]() {
    state ^= state << 13;
    state ^= state >> 7;
    state ^= state << 17;
    return state;
  };

  for (int trial = 0; trial < 60; ++trial) {
    std::string left_text;
    std::string right_text;
    std::vector<std::string> left_lines;
    std::vector<std::string> right_lines;
    const std::size_t count = 3 + (next() % 10);
    for (std::size_t i = 0; i < count; ++i) {
      left_lines.push_back(alphabet[next() % alphabet.size()]);
      right_lines.push_back(alphabet[next() % alphabet.size()]);
      if (i != 0) {
        left_text += '\n';
        right_text += '\n';
      }
      left_text += left_lines.back();
      right_text += right_lines.back();
    }

    for (const bool ignore_whitespace : {false, true}) {
      CompareBuildOptions options;
      options.ignore_whitespace = ignore_whitespace;
      const CompareModel model = BuildCompareModel(left_text, right_text, options);

      for (const CompareRow& row : model.rows) {
        if (row.kind != CompareRowKind::Unchanged) {
          continue;
        }
        const bool equal = ignore_whitespace
                               ? strip(row.left_text) == strip(row.right_text)
                               : row.left_text == row.right_text;
        Expect(equal,
               ignore_whitespace
                   ? "an Unchanged row must be equal after stripping whitespace"
                   : "an Unchanged row must be byte-identical when whitespace matters");
      }
    }
  }

  {
    CompareBuildOptions ignore_ws;
    ignore_ws.ignore_whitespace = true;
    const CompareModel folded = BuildCompareModel("x\n a b \ny", "x\nab\ny", ignore_ws);
    Expect(folded.rows.size() == 3, "whitespace-folded lines align one-to-one");
    for (const CompareRow& row : folded.rows) {
      Expect(row.kind == CompareRowKind::Unchanged,
             "lines equal after whitespace removal are Unchanged under ignore_whitespace");
    }
    const CompareModel exact = BuildCompareModel("x\n a b \ny", "x\nab\ny", CompareBuildOptions{});
    Expect(std::any_of(exact.rows.begin(), exact.rows.end(),
                       [](const CompareRow& row) { return row.kind != CompareRowKind::Unchanged; }),
           "the same pair must NOT collapse when whitespace is significant");

    const CompareModel blanks = BuildCompareModel("a\n   \nb", "a\n\nb", ignore_ws);
    Expect(blanks.rows.size() == 3, "a blank and a whitespace-only line align");
    for (const CompareRow& row : blanks.rows) {
      Expect(row.kind == CompareRowKind::Unchanged,
             "whitespace-only vs empty is Unchanged under ignore_whitespace");
    }

    const std::string fine_left =
        "shared-head\nLEFT-ONLY-A\n\tws-alpha\n a\tb \nx\x0by\nu\x0cv\n"
        "LEFT-ONLY-B\nshared-tail";
    const std::string fine_right =
        "shared-head\nRIGHT-ONLY-A\nws-alpha\n  a b\nx y\nu  v\n"
        "RIGHT-ONLY-B\nshared-tail";
    const CompareModel fine = BuildCompareModel(fine_left, fine_right, ignore_ws);
    int folded_unchanged = 0;
    for (const CompareRow& row : fine.rows) {
      if (row.kind == CompareRowKind::Unchanged && row.left_text != row.right_text) {
        ++folded_unchanged;
      }
    }
    Expect(folded_unchanged == 4,
           "all four whitespace-only-variant middle lines must fold to Unchanged — "
           "every in-line whitespace class has to be stripped, not just the space");
  }
}

void TestBuildCompareModelIntoMatchesAFreshBuild() {
  const auto rows_equal = [](const CompareModel& a, const CompareModel& b) {
    if (a.rows.size() != b.rows.size() || a.hunks.size() != b.hunks.size()) {
      return false;
    }
    for (std::size_t i = 0; i < a.rows.size(); ++i) {
      const CompareRow& lhs = a.rows[i];
      const CompareRow& rhs = b.rows[i];
      if (lhs.left_text != rhs.left_text || lhs.right_text != rhs.right_text ||
          lhs.left_line != rhs.left_line || lhs.right_line != rhs.right_line ||
          lhs.kind != rhs.kind || lhs.hunk != rhs.hunk ||
          lhs.left_changed_spans.size() != rhs.left_changed_spans.size() ||
          lhs.right_changed_spans.size() != rhs.right_changed_spans.size()) {
        return false;
      }
    }
    for (std::size_t i = 0; i < a.hunks.size(); ++i) {
      if (a.hunks[i].index != b.hunks[i].index || a.hunks[i].start_row != b.hunks[i].start_row ||
          a.hunks[i].end_row != b.hunks[i].end_row) {
        return false;
      }
    }
    return true;
  };

  struct Case {
    std::string left;
    std::string right;
  };
  const std::vector<Case> cases = {
      {"a1\na2\na3\na4\na5\na6\na7\na8\n", "b1\nb2\nb3\nb4\nb5\nb6\nb7\nb8\n"},
      {"one\ntwo\n", "one\ninserted\ntwo\n"},
      {"same\nsame\n", "same\nsame\n"},
      {"alpha bravo\n", "alpha charlie\n"},
      {"only left\n", ""},
      {"x1\nx2\nx3\nx4\nx5\nx6\nx7\nx8\nx9\n", "y1\ny2\ny3\ny4\ny5\ny6\ny7\ny8\ny9\n"},
  };

  CompareModel recycled;
  for (const Case& test_case : cases) {
    BuildCompareModelInto(recycled, test_case.left, test_case.right, CompareBuildOptions{});
    const CompareModel fresh =
        BuildCompareModel(test_case.left, test_case.right, CompareBuildOptions{});
    Expect(rows_equal(recycled, fresh),
           "a recycled rebuild must equal a fresh build of the same inputs");
    Expect(recycled.left_final_newline_missing == fresh.left_final_newline_missing &&
               recycled.right_final_newline_missing == fresh.right_final_newline_missing &&
               recycled.left_uses_crlf == fresh.left_uses_crlf &&
               recycled.right_uses_crlf == fresh.right_uses_crlf,
           "the whole-model flags are rebuilt too, not carried over");
  }
}

void TestBuildCompareModelIntoReusesRowStorage() {
  std::string left;
  std::string right;
  for (int i = 0; i < 200; ++i) {
    left += "line " + std::to_string(i) + " left\n";
    right += "line " + std::to_string(i) + " right\n";
  }

  const compare::CompareTextBuffer left_buffer = compare::MakeCompareText(left);
  const compare::CompareTextBuffer right_buffer = compare::MakeCompareText(right);

  CompareModel model;
  BuildCompareModelInto(model, left_buffer, right_buffer, CompareBuildOptions{});
  Expect(model.rows.size() >= 200, "the fixture produces a large row set");
  const CompareRow* const first_rows_data = model.rows.data();
  const std::size_t first_rows_capacity = model.rows.capacity();

  BuildCompareModelInto(model, left_buffer, right_buffer, CompareBuildOptions{});
  Expect(model.rows.data() == first_rows_data && model.rows.capacity() == first_rows_capacity,
         "the row vector's storage is reused on a rebuild, not reallocated");
  Expect(model.left_source == left_buffer && model.right_source == right_buffer,
         "an adopted buffer is shared, not copied");
}

void TestCompareModelBuildScratchIsRetainedButNotPartOfTheValue() {
  CompareModel model;
  Expect(model.build_scratch.get() == nullptr, "a model starts with no build scratch");

  BuildCompareModelInto(model, "a\nb\nc\n", "a\nB\nc\n", CompareBuildOptions{});
  const auto* const first_scratch = model.build_scratch.get();
  Expect(first_scratch != nullptr, "the first rebuild creates the scratch");

  BuildCompareModelInto(model, "a\nb\nc\nd\n", "a\nB\nc\nD\n", CompareBuildOptions{});
  Expect(model.build_scratch.get() == first_scratch,
         "later rebuilds reuse the scratch rather than recreating it");

  const CompareModel copy = model;
  Expect(copy.build_scratch.get() == nullptr,
         "a copied model must not share or duplicate the scratch");
  Expect(copy.rows.size() == model.rows.size(),
         "the copy is still a complete model — only the scratch is left behind");

  CompareModel moved = std::move(model);
  Expect(moved.build_scratch.get() == first_scratch, "a move transfers the scratch");

  CompareModel rebuilt_copy = copy;
  BuildCompareModelInto(rebuilt_copy, "one\ntwo\n", "one\ntwo\nthree\n", CompareBuildOptions{});
  const CompareModel fresh =
      BuildCompareModel("one\ntwo\n", "one\ntwo\nthree\n", CompareBuildOptions{});
  Expect(rebuilt_copy.rows.size() == fresh.rows.size(),
         "rebuilding into a scratch-less copy produces the same model");
  for (std::size_t i = 0; i < fresh.rows.size(); ++i) {
    Expect(rebuilt_copy.rows[i].left_text == fresh.rows[i].left_text &&
               rebuilt_copy.rows[i].right_text == fresh.rows[i].right_text &&
               rebuilt_copy.rows[i].kind == fresh.rows[i].kind,
           "every row of the rebuilt copy matches the fresh build");
  }
}

void TestCompareModelRowsSurviveCopyMoveAndTemporaryInputs() {
  CompareModel model = BuildCompareModel(std::string("alpha\nbeta\n"),
                                         std::string("alpha\nbeta2\n"));
  Expect(model.rows.size() >= 2, "at least the two content rows");
  Expect(model.rows[0].left_text == "alpha" && model.rows[1].right_text == "beta2",
         "rows read correctly after the inputs are gone");

  const auto inside = [](std::string_view view, const std::string& buffer) {
    if (view.empty()) {
      return true;
    }
    return view.data() >= buffer.data() && view.data() + view.size() <= buffer.data() + buffer.size();
  };
  for (const CompareRow& row : model.rows) {
    Expect(inside(row.left_text, *model.left_source), "left_text views the model's left source");
    Expect(inside(row.right_text, *model.right_source), "right_text views the model's right source");
  }

  CompareModel copied = model;
  const CompareModel moved = std::move(model);
  Expect(copied.rows[1].right_text == "beta2", "a copied model's rows still read");
  Expect(moved.rows[1].right_text == "beta2", "a moved model's rows still read");
  Expect(copied.left_source == moved.left_source,
         "a copy shares the source buffer rather than duplicating it");
  Expect(moved.rows[0].left_text.data() == copied.rows[0].left_text.data(),
         "a move leaves the row views pointing at the same bytes");
}


void TestCompareLineDiffOpsMatchWeightedLcsOracle() {
  using microide::compare::BuildLineDiffOps;
  using microide::compare::DiffOp;
  using microide::compare::DiffOpKind;
  static constexpr std::string_view kAlphabet[] = {"a", "bb", "ccc", "dddddddd", "e"};
  std::mt19937 rng(20260905u);
  const auto random_doc = [&](std::size_t max_lines) {
    std::vector<std::string_view> lines(rng() % (max_lines + 1));
    for (auto& line : lines) {
      line = kAlphabet[rng() % std::size(kAlphabet)];
    }
    return lines;
  };
  const auto weight_of = [](std::string_view line, const std::vector<std::string_view>& a,
                            const std::vector<std::string_view>& b) {
    const std::size_t occurrences = static_cast<std::size_t>(std::count(a.begin(), a.end(), line)) +
                                    static_cast<std::size_t>(std::count(b.begin(), b.end(), line));
    return std::max<std::size_t>(1, (line.size() + 8) / std::max<std::size_t>(1, occurrences));
  };
  const auto trim_common = [](std::vector<std::string_view>& a, std::vector<std::string_view>& b) {
    std::size_t prefix = 0;
    while (prefix < a.size() && prefix < b.size() && a[prefix] == b[prefix]) ++prefix;
    std::size_t suffix = 0;
    while (a.size() - suffix > prefix && b.size() - suffix > prefix &&
           a[a.size() - 1 - suffix] == b[b.size() - 1 - suffix]) {
      ++suffix;
    }
    a = std::vector<std::string_view>(a.begin() + static_cast<std::ptrdiff_t>(prefix),
                                      a.end() - static_cast<std::ptrdiff_t>(suffix));
    b = std::vector<std::string_view>(b.begin() + static_cast<std::ptrdiff_t>(prefix),
                                      b.end() - static_cast<std::ptrdiff_t>(suffix));
    return prefix + suffix;
  };
  const auto best_score = [&](const std::vector<std::string_view>& a,
                              const std::vector<std::string_view>& b) {
    std::vector<std::vector<std::size_t>> dp(a.size() + 1, std::vector<std::size_t>(b.size() + 1));
    for (std::size_t i = 1; i <= a.size(); ++i) {
      for (std::size_t j = 1; j <= b.size(); ++j) {
        dp[i][j] = std::max(dp[i - 1][j], dp[i][j - 1]);
        if (a[i - 1] == b[j - 1]) {
          dp[i][j] = std::max(dp[i][j], dp[i - 1][j - 1] + weight_of(a[i - 1], a, b));
        }
      }
    }
    return dp[a.size()][b.size()];
  };

  int mismatches = 0;
  for (int iteration = 0; iteration < 600 && mismatches == 0; ++iteration) {
    const std::vector<std::string_view> left = random_doc(18);
    const std::vector<std::string_view> right = random_doc(18);
    const std::vector<DiffOp> ops = BuildLineDiffOps(left, right);
    std::vector<std::string_view> middle_left = left;
    std::vector<std::string_view> middle_right = right;
    const std::size_t trimmed = trim_common(middle_left, middle_right);

    std::vector<std::string_view> replayed_left;
    std::vector<std::string_view> replayed_right;
    std::size_t score = 0;
    bool equal_pairs_match = true;
    for (const DiffOp& op : ops) {
      switch (op.kind) {
        case DiffOpKind::Equal:
          score += weight_of(op.text, middle_left, middle_right);
          equal_pairs_match = equal_pairs_match && op.text == op.right_text;
          replayed_left.push_back(op.text);
          replayed_right.push_back(op.right_text);
          break;
        case DiffOpKind::Delete:
          replayed_left.push_back(op.text);
          break;
        case DiffOpKind::Insert:
          replayed_right.push_back(op.text);
          break;
      }
    }
    std::size_t trimmed_score = 0;
    {
      const std::size_t prefix = [&] {
        std::size_t p = 0;
        while (p < left.size() && p < right.size() && left[p] == right[p]) ++p;
        return p;
      }();
      const std::size_t suffix = trimmed - prefix;
      for (std::size_t i = 0; i < prefix; ++i) trimmed_score += weight_of(left[i], middle_left, middle_right);
      for (std::size_t i = 0; i < suffix; ++i) {
        trimmed_score += weight_of(left[left.size() - 1 - i], middle_left, middle_right);
      }
    }
    const std::size_t optimum = best_score(middle_left, middle_right);
    if (replayed_left != left || replayed_right != right || !equal_pairs_match ||
        score - trimmed_score != optimum) {
      ++mismatches;
      std::string detail = "left=";
      for (auto l : left) { detail += '['; detail += l; detail += ']'; }
      detail += " right=";
      for (auto r : right) { detail += '['; detail += r; detail += ']'; }
      detail += " score=" + std::to_string(score - trimmed_score) +
                " optimum=" + std::to_string(optimum);
      Expect(false, "line diff ops must replay both sides at the optimal weighted score: " + detail);
    }
  }
  Expect(mismatches == 0, "line diff ops agree with the weighted-LCS oracle");
}

void TestFinalNewlineOnlyChangeRowPointsAtItsHunk() {
  const CompareModel added = BuildCompareModel("alpha\nbeta", "alpha\nbeta\n");
  Expect(added.hunks.size() == 1, "adding a trailing newline is one hunk");
  const CompareRow& added_row = added.rows[added.hunks[0].start_row];
  Expect(added_row.kind != CompareRowKind::Unchanged, "the phantom row is a change");
  Expect(added_row.hunk == 0,
         "the phantom row must name its hunk, got " + std::to_string(added_row.hunk));

  const CompareModel removed = BuildCompareModel("alpha\nbeta\n", "alpha\nbeta");
  Expect(removed.hunks.size() == 1, "removing a trailing newline is one hunk");
  const CompareRow& removed_row = removed.rows[removed.hunks[0].start_row];
  Expect(removed_row.hunk == 0,
         "the phantom row must name its hunk, got " + std::to_string(removed_row.hunk));
}

void TestFinalNewlineChangeAdjacentToAHunkJoinsIt() {
  const CompareModel model = BuildCompareModel("alpha\nbeta\n", "alpha\ngamma");
  Expect(model.hunks.size() == 1,
         "the edited last line and the newline change are one hunk, got " +
             std::to_string(model.hunks.size()));
  const auto& hunk = model.hunks[0];
  for (int row = hunk.start_row; row <= hunk.end_row; ++row) {
    Expect(model.rows[row].hunk == hunk.index,
           "every row the hunk covers must name it, row " + std::to_string(row) + " said " +
               std::to_string(model.rows[row].hunk));
  }
}

void RegisterCompareModelTests(std::vector<TestCase>& tests) {
  AddTest(tests, "Compare/FinalNewlineOnlyChangeRowPointsAtItsHunk",
          TestFinalNewlineOnlyChangeRowPointsAtItsHunk);
  AddTest(tests, "Compare/FinalNewlineChangeAdjacentToAHunkJoinsIt",
          TestFinalNewlineChangeAdjacentToAHunkJoinsIt);
  AddTest(tests, "CompareModel/LineDiffOpsMatchWeightedLcsOracle",
          TestCompareLineDiffOpsMatchWeightedLcsOracle);
  AddTest(tests, "CompareModel/BuildCompareModelIntoMatchesAFreshBuild",
          TestBuildCompareModelIntoMatchesAFreshBuild);
  AddTest(tests, "CompareModel/BuildCompareModelIntoReusesRowStorage",
          TestBuildCompareModelIntoReusesRowStorage);
  AddTest(tests, "CompareModel/BuildScratchIsRetainedButNotPartOfTheValue",
          TestCompareModelBuildScratchIsRetainedButNotPartOfTheValue);
  AddTest(tests, "CompareModel/RowsSurviveCopyMoveAndTemporaryInputs",
          TestCompareModelRowsSurviveCopyMoveAndTemporaryInputs);
  AddTest(tests, "CompareModel/LineEqualityInterningMatchesPredicate",
          TestCompareLineEqualityInterningMatchesPredicate);
  AddTest(tests, "Compare/IntralineBudgetBoundsLargeModifiedHunk",
          TestCompareLargeRewrittenHunkStaysBoundedAndNovel);
  AddTest(tests, "Compare/ManyTokenLineBoundsAlignmentDp",
          TestCompareManyTokenLineBoundsAlignmentDp);
  AddTest(tests, "Compare/LongLineSkipsIntralineRefinement",
          TestCompareLongLineSkipsIntralineRefinement);
  AddTest(tests, "Compare/IgnoreWhitespacePreservesRightText",
          TestCompareIgnoreWhitespacePreservesRightText);
  AddTest(tests, "Compare/IgnoreWhitespaceInteriorHunkLine",
          TestCompareIgnoreWhitespaceInteriorHunkLine);
  AddTest(tests, "Compare/SimpleFixture", TestCompareSimpleFixture);
  AddTest(tests, "Compare/CodeFixture", TestCompareCodeFixture);
  AddTest(tests, "Compare/AsciiChangedSpans", TestCompareAsciiChangedSpans);
  AddTest(tests, "Compare/Utf8ChangedSpans", TestCompareUtf8ChangedSpans);
  AddTest(tests, "Compare/CodeTokenChangedSpans", TestCompareCodeTokenChangedSpans);
  AddTest(tests, "Compare/AssignmentPrefixStaysUnchanged",
          TestCompareAssignmentPrefixStaysUnchanged);
  AddTest(tests, "Compare/LongQueryChangedSpansKeepSharedClause",
          TestCompareLongQueryChangedSpansKeepSharedClause);
  AddTest(tests, "Compare/RepeatedStructureKeepsSharedBlockUnchanged",
          TestCompareRepeatedStructureKeepsSharedBlockUnchanged);
  AddTest(tests, "Compare/ImportExpansionKeepsFollowingImportsUnchanged",
          TestCompareImportExpansionKeepsFollowingImportsUnchanged);
  AddTest(tests, "Compare/ContextAwareAlignment", TestCompareContextAwareAlignment);
  AddTest(tests, "Compare/ModelPreservesBothSidesRoundTrip",
          TestCompareModelPreservesBothSidesRoundTrip);
  AddTest(tests, "Compare/AnchoredFallbackMatchesEveryUniqueAnchor",
          TestCompareModelAnchoredFallbackMatchesEveryUniqueAnchor);
  AddTest(tests, "Compare/IntralineFallbackCoversChangedRegion",
          TestCompareIntralineFallbackCoversChangedRegion);
  AddTest(tests, "Compare/LargeInputsUseBoundedFallback",
          TestCompareLargeInputsUseBoundedFallback);
  AddTest(tests, "Compare/ReversedUniqueLinesStayBounded",
          TestCompareReversedUniqueLinesStayBounded);
  AddTest(tests, "Compare/LargePaddedAssignmentPrefixStaysUnchanged",
          TestCompareLargePaddedAssignmentPrefixStaysUnchanged);
  AddTest(tests, "Compare/LargeIdenticalInputsStayUnchanged",
          TestCompareLargeIdenticalInputsStayUnchanged);
  AddTest(tests, "Compare/ProfiledBuildMatchesUnprofiledModel",
          TestProfiledCompareBuildMatchesUnprofiledModel);
}

}
