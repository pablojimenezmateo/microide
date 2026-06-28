#include "TestSupport.h"

#include "compare/CompareModel.h"
#include "util/StringUtil.h"

#include <algorithm>
#include <sstream>
#include <string>
#include <string_view>

namespace microide::tests {
namespace {

using microide::compare::BuildCompareModel;
using microide::compare::BuildCompareModelProfiled;
using microide::compare::CompareBuildOptions;
using microide::compare::CompareModel;
using microide::compare::CompareRow;
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
         "utf8 left changed span should isolate the original emoji");
  Expect(SpanText(row.right_text, right_span) == "😃",
         "utf8 right changed span should isolate the replacement emoji");
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
    if (row.kind == CompareRowKind::Deleted &&
        row.left_text == "import { ordersQuery } from '@example/business/shared_queries'") {
      saw_shared_query_deleted = true;
    }
    if (row.kind == CompareRowKind::Added &&
        row.right_text == "  ordersQueryWithoutLines") {
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
         ("import expansion diff should keep the single-line shared query import deleted\n" +
          DescribeRows(model, 6, 12))
             .c_str());
  Expect(saw_shared_query_added,
         "import expansion diff should keep the extra shared query symbol as added");
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

// Reconstruct one side of the original input from the model rows: every row that
// carries a line on that side (line number != 0) contributes its text, in order.
std::vector<std::string> ReconstructSide(const CompareModel& model, bool left) {
  std::vector<std::string> lines;
  for (const auto& row : model.rows) {
    if (left ? (row.left_line != 0) : (row.right_line != 0)) {
      lines.push_back(left ? row.left_text : row.right_text);
    }
  }
  return lines;
}

// Core "SHALL NOT degrade by file size" guarantee: regardless of which internal
// path (exact LCS, anchored fallback, hunk-alignment fallback) runs, the model
// must reproduce BOTH input sides line-for-line. No truncation, no dropped hunks.
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

  // Exceeds kMaxLineLcsMatrixCells (250k) and kMaxHunkAlignmentMatrixCells (65k):
  // forces the anchored line fallback and the hunk-alignment fallback together.
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

  // Interleaved shared anchors inside an oversized region: the anchored fallback
  // must still place every unique anchor and reproduce the lines around it.
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

// When a modified row is long enough to exceed the intra-line LCS guards, the
// span population skips the fine LCS but must still cover the changed region and
// stay on UTF-8 boundaries (no crash, no degraded correctness).
void TestCompareIntralineFallbackCoversChangedRegion() {
  std::string left;
  std::string right;
  // ~600 distinct tokens per side -> (600+1)^2 > kMaxIntralineLcsMatrixCells.
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
                                         profiled.profile.hunk_alignment_ns +
                                         profiled.profile.intraline_ns,
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

// Regression: under ignore_whitespace, two sides can compare equal while their
// whitespace differs. The all-equal fast path must still show each column's own
// text — it previously copied the left line into right_text.
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

// Regression: a whitespace-only-different line interior to a changed hunk (not on
// the matched prefix/suffix, so it flows through the LCS line-op builder) must be
// Unchanged under ignore_whitespace. It previously rendered Modified because the
// LCS used raw `==` instead of the whitespace-aware comparator, so the toggle
// silently no-opped interior lines. The right column must still show the right
// file's own (un-trimmed) text.
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

  // Without the toggle the same interior line is a real Modified change.
  const CompareModel exact = BuildCompareModel(left, right);
  const CompareRow* mid_exact = FindRowByRightText(exact, "mid");
  Expect(mid_exact != nullptr && mid_exact->kind == CompareRowKind::Modified,
         "without ignore_whitespace the interior line stays Modified");
}

}  // namespace

void RegisterCompareModelTests(std::vector<TestCase>& tests) {
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
  AddTest(tests, "Compare/IntralineFallbackCoversChangedRegion",
          TestCompareIntralineFallbackCoversChangedRegion);
  AddTest(tests, "Compare/LargeInputsUseBoundedFallback",
          TestCompareLargeInputsUseBoundedFallback);
  AddTest(tests, "Compare/LargePaddedAssignmentPrefixStaysUnchanged",
          TestCompareLargePaddedAssignmentPrefixStaysUnchanged);
  AddTest(tests, "Compare/LargeIdenticalInputsStayUnchanged",
          TestCompareLargeIdenticalInputsStayUnchanged);
  AddTest(tests, "Compare/ProfiledBuildMatchesUnprofiledModel",
          TestProfiledCompareBuildMatchesUnprofiledModel);
}

}  // namespace microide::tests
