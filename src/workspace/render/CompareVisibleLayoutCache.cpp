#include "workspace/render/CompareVisibleLayoutCache.h"

#include <algorithm>
#include <string_view>
#include <utility>

#include "workspace/git/CompareTabReview.h"

namespace microide::workspace {
namespace {

constexpr std::size_t kMaxCompareLayoutBytes = 1u << 20;  // 1 MiB

static_assert((CompareVisibleLayoutCache::kTableSize &
               (CompareVisibleLayoutCache::kTableSize - 1)) == 0,
              "the index masks with kTableSize - 1, so it must be a power of two");
static_assert(CompareVisibleLayoutCache::kTableSize >= 2 * CompareVisibleLayoutCache::kLimit,
              "linear probing only terminates while the table keeps a free position");

// Probe the open-addressed index for `key`. Returns the table position holding
// it, or the first empty position if it is absent — `found` says which. Linear
// probing is safe to run unguarded because `live_count` is capped at half the
// table size, so an empty position always exists.
struct LayoutIndexProbe {
  std::size_t position = 0;
  bool found = false;
};

LayoutIndexProbe ProbeLayoutIndex(const CompareVisibleLayoutCache& cache,
                                  const CompareVisibleLayoutCacheKey& key) {
  constexpr std::size_t kMask = CompareVisibleLayoutCache::kTableSize - 1;
  std::size_t position = CompareVisibleLayoutCacheKeyHash{}(key) & kMask;
  for (;;) {
    const std::uint32_t stored = cache.table[position];
    if (stored == 0) {
      return {.position = position, .found = false};
    }
    const std::size_t slot = stored - 1;
    if (slot < cache.live_count && cache.keys[slot] == key) {
      return {.position = position, .found = true};
    }
    position = (position + 1) & kMask;
  }
}

}  // namespace

void ResetCompareVisibleLayoutCache(CompareTabState& compare_tab) {
  CompareVisibleLayoutCache& cache = compare_tab.visible_layouts;
  // Deliberately neither `layouts.clear()` nor an index rebuild: the slab keeps
  // its `LayoutLine` storage so the next build refills it in place, and the
  // table is zeroed rather than freed. See the type comment in
  // `WorkspaceTabState.h` (TD-2026-08-17-261).
  cache.live_count = 0;
  std::fill(cache.table.begin(), cache.table.end(), 0u);
}

void PrepareCompareVisibleLayoutsForWindow(CompareTabState& compare_tab,
                                           std::size_t visual_start_row,
                                           std::size_t visual_end_row,
                                           std::size_t left_visible_columns,
                                           std::size_t right_visible_columns) {
  CompareVisibleLayoutCache& cache = compare_tab.visible_layouts;
  if (cache.table.size() != CompareVisibleLayoutCache::kTableSize) {
    cache.table.assign(CompareVisibleLayoutCache::kTableSize, 0u);
    cache.live_count = 0;
  }
  if (cache.model_revision != compare_tab.model_revision) {
    cache.model_revision = compare_tab.model_revision;
    ResetCompareVisibleLayoutCache(compare_tab);
  }

  const std::size_t tab_size = compare_tab.right_viewport.tab_size();
  const std::size_t clamped_end = std::min(visual_end_row, CompareTabVisualRowCount(compare_tab));
  if (visual_start_row >= clamped_end) {
    return;
  }

  const auto ensure_layout = [&](std::size_t model_row,
                                 bool right_side,
                                 std::size_t row_visual_start,
                                 std::size_t visible_columns,
                                 std::string_view text) {
    if (text.size() > kMaxCompareLayoutBytes || visible_columns == 0) {
      return;
    }
    const CompareVisibleLayoutCacheKey key{
        .model_row = model_row,
        .horizontal_scroll = row_visual_start,
        .visible_columns = visible_columns,
        .tab_size = tab_size,
        .right_side = right_side,
    };
    LayoutIndexProbe probe = ProbeLayoutIndex(cache, key);
    if (probe.found) {
      return;
    }
    if (cache.live_count >= CompareVisibleLayoutCache::kLimit) {
      ResetCompareVisibleLayoutCache(compare_tab);
      probe = ProbeLayoutIndex(cache, key);
    }
    const std::size_t slot = cache.live_count;
    if (slot == cache.layouts.size()) {
      cache.layouts.emplace_back();
      cache.keys.emplace_back();
    }
    editor::TextLayout::BuildVisibleLineInto(text, row_visual_start, visible_columns, tab_size,
                                             cache.layouts[slot]);
    cache.keys[slot] = key;
    cache.table[probe.position] = static_cast<std::uint32_t>(slot + 1);
    cache.live_count = slot + 1;
  };

  const bool wrapped = compare_tab.wrap_layout.active();
  for (std::size_t visual_index = visual_start_row; visual_index < clamped_end; ++visual_index) {
    const DiffWrapRow wrap_row = compare_tab.wrap_layout.RowAt(visual_index);
    const compare::ComparePresentationRow* presentation_row =
        CompareTabPresentationRowAt(compare_tab, wrap_row.unit);
    if (presentation_row == nullptr ||
        presentation_row->kind != compare::ComparePresentationRowKind::Model ||
        presentation_row->model_row_index >= compare_tab.model.rows.size()) {
      continue;
    }
    const std::size_t model_row = presentation_row->model_row_index;
    const compare::CompareRow& row = compare_tab.model.rows[model_row];
    if (row.left_line > 0 && wrap_row.left_present) {
      const std::size_t start = wrapped ? wrap_row.left_start : compare_tab.horizontal_scroll;
      const std::size_t columns =
          wrapped ? wrap_row.left_end - wrap_row.left_start : left_visible_columns;
      ensure_layout(model_row, false, start, columns, row.left_text);
    }
    if (row.right_line > 0 && wrap_row.right_present) {
      const std::size_t start = wrapped ? wrap_row.right_start : compare_tab.horizontal_scroll;
      const std::size_t columns =
          wrapped ? wrap_row.right_end - wrap_row.right_start : right_visible_columns;
      ensure_layout(model_row, true, start, columns, row.right_text);
    }
  }
}

const editor::LayoutLine* CompareVisibleLayoutForRow(const CompareTabState& compare_tab,
                                                     std::size_t model_row,
                                                     bool right_side,
                                                     std::size_t row_visual_start,
                                                     std::size_t visible_columns) {
  const CompareVisibleLayoutCache& cache = compare_tab.visible_layouts;
  if (cache.model_revision != compare_tab.model_revision || cache.live_count == 0 ||
      cache.table.size() != CompareVisibleLayoutCache::kTableSize) {
    return nullptr;
  }
  const CompareVisibleLayoutCacheKey key{
      .model_row = model_row,
      .horizontal_scroll = row_visual_start,
      .visible_columns = visible_columns,
      .tab_size = compare_tab.right_viewport.tab_size(),
      .right_side = right_side,
  };
  const LayoutIndexProbe probe = ProbeLayoutIndex(cache, key);
  if (!probe.found) {
    return nullptr;
  }
  return &cache.layouts[cache.table[probe.position] - 1];
}

}  // namespace microide::workspace
