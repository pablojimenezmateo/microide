// Performance scenarios that close coverage gaps left by the TD-2026-07-17A
// tech-debt burndown (see dev-docs/project/known-tech-debt.md).
//
// The burndown rewrote a set of correctness-preserving-but-perf-sensitive hot
// paths -- mostly O(n^2) -> indexed/hashed lookups (cluster 3) and the
// coordinate/cross-boundary rewrites (cluster 3b) -- but several of those hot
// functions had NO perf scenario exercising them at scale, so a
// tools/perf-compare.py run against main could not have caught an accidental
// return to quadratic behavior. Each scenario below drives one of those
// rewritten hot paths at a scale where the algorithmic complexity dominates, so
// the reference-runner baseline pins it.
//
// All of these are pure-unit micro-benchmarks (they construct the real data
// structures directly and call the hot function in a loop, ignoring the app
// driver) except the snippet one, which needs the live snippet session. The
// pure-unit ones are deterministic and allocation-stable, so they are gated
// (smoke = true, baseline_gated = true) with committed reference-runner
// baselines, matching the promotion path in dev-docs/performance/perf-harness.md.
#include "perf/PerfHarness.h"

#include <cstddef>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <utility>
#include <vector>

#include "editor/SnippetEngine.h"
#include "editor/TextViewport.h"
#include "editor/TextViewportInternal.h"
#include "plugin/PluginHost.h"
#include "plugin/PluginRegistryInterop.h"
#include "util/TextFileIO.h"
#include "workspace/AssistProviderMerge.h"
#include "workspace/SettingsOverlayService.h"
#include "workspace/WorkspacePersistenceFormat.h"
#include "workspace/WorkspaceReviewComments.h"
#include "workspace/WorkspaceSettingsRegistry.h"

namespace microide::tests::perf {
namespace {

// ---- 114: assist_merge::RankedUnion ---------------------------------------
//
// The concurrent LSP + plugin completion/code-action merge de-dups two result
// lists by key. Providers are bounded only by harvest caps (LSP up to 5,000
// rows, plugins up to 20,000), so the fix switched the linear seen-scan to a
// hash set above a small combined-size threshold. This benches the union of two
// large label lists with ~50% key overlap: with the hash set it is O(total);
// reverting to the linear scan makes it O(total^2) -- tens of millions of
// comparisons -- which the baseline would flag immediately.
struct RankedUnionItem {
  std::string label;
  int rank = 0;
};

void RunAssistRankedUnionMerge(ScenarioContext& context) {
  constexpr int kPrimary = 6000;
  constexpr int kSecondary = 6000;
  std::vector<RankedUnionItem> primary;
  std::vector<RankedUnionItem> secondary;
  primary.reserve(kPrimary);
  secondary.reserve(kSecondary);
  for (int i = 0; i < kPrimary; ++i) {
    primary.push_back({"symbol_" + std::to_string(i), i});
  }
  // Overlap the lower half of the key space so de-dup actually rejects entries,
  // and add fresh keys in the upper half so both sources contribute.
  for (int i = 0; i < kSecondary; ++i) {
    secondary.push_back({"symbol_" + std::to_string(i / 2 + kPrimary / 2), i});
  }
  const auto key_of = [](const RankedUnionItem& item) { return item.label; };
  context.Measure("assist.ranked_union", [&]() {
    for (int iter = 0; iter < 40; ++iter) {
      std::vector<RankedUnionItem> merged =
          workspace::assist_merge::RankedUnion(primary, secondary, key_of);
      volatile std::size_t sink = merged.size();
      (void)sink;
    }
  });
}

// ---- 063: ReviewCommentsRegistry per-URI marker index ----------------------
//
// The inline-review render pass resolves each visible document's markers via
// MarkersForUri, then tests individual lines with HasMarkerAtLine. The fix made
// IndexForUri non-inserting (a marker-free document does not grow the cache) and
// RebuildIndices a single O(comments+threads) pass. This benches a marker sweep
// over many documents -- including marker-free ones -- so a regression to the
// old per-first-seen-URI full rescan (or the empty-URI cache inserts) shows up
// as an allocation / wall blow-up.
void RunReviewCommentsRegistryLookup(ScenarioContext& context) {
  workspace::ReviewCommentsRegistry registry;
  constexpr int kUris = 300;
  constexpr int kCommentsPerUri = 20;
  std::vector<std::string> uris;
  uris.reserve(kUris);
  for (int u = 0; u < kUris; ++u) {
    std::string uri = "file:///repo/src/module_" + std::to_string(u) + ".cpp";
    uris.push_back(uri);
    for (int c = 0; c < kCommentsPerUri; ++c) {
      workspace::ReviewComment comment;
      comment.id = "c_" + std::to_string(u) + "_" + std::to_string(c);
      comment.uri = uri;
      comment.line = c * 7 + 1;
      comment.author = "reviewer";
      comment.body = "needs a second look here";
      registry.AddComment(comment);
    }
  }
  // Half the swept URIs carry no markers at all -- these must stay out of the
  // cache (non-inserting IndexForUri), which is the crux of the fix.
  std::vector<std::string> marker_free;
  marker_free.reserve(kUris);
  for (int u = 0; u < kUris; ++u) {
    marker_free.push_back("file:///repo/other/blank_" + std::to_string(u) + ".cpp");
  }

  context.Measure("review_comments.marker_sweep", [&]() {
    volatile int hits = 0;
    for (int iter = 0; iter < 30; ++iter) {
      for (const std::string& uri : uris) {
        const auto markers = registry.MarkersForUri(uri);
        if (markers) {
          for (int line = 1; line <= 140; line += 1) {
            hits += markers.HasMarkerAtLine(line) ? 1 : 0;
          }
        }
      }
      for (const std::string& uri : marker_free) {
        const auto markers = registry.MarkersForUri(uri);
        hits += static_cast<bool>(markers) ? 1 : 0;
      }
    }
    (void)hits;
  });
}

#if MICROIDE_HAS_LUA_PLUGINS
// ---- 081: registry_interop::ApplyStatusItemUpdate id->pos cache ------------
//
// A plugin firing frequent ctx.status.update calls resolves its target item in
// the published status-item order vector. The fix keeps a caller-owned id->pos
// cache alongside the vector (rebuilt only on size mismatch) so steady-state
// updates are O(1) instead of rescanning a registry capped at 100k items. This
// benches many scattered updates against a large registry: O(1) with the cache,
// O(items) per update without it.
void RunPluginStatusItemUpdate(ScenarioContext& context) {
  constexpr int kItems = 4000;
  std::vector<plugin::PluginHost::ContributedStatusItem> order;
  order.reserve(kItems);
  for (int i = 0; i < kItems; ++i) {
    plugin::PluginHost::ContributedStatusItem item;
    item.id = "plugin.status.item_" + std::to_string(i);
    item.text = "idle";
    item.plugin_id = "perf.plugin";
    order.push_back(std::move(item));
  }
  std::unordered_map<std::string, std::size_t> index;

  context.Measure("status_registry.apply_update", [&]() {
    for (int iter = 0; iter < 8; ++iter) {
      for (int i = 0; i < kItems; ++i) {
        plugin::registry_interop::StatusItemUpdate update;
        // Scatter the target so a linear-scan regression pays the full walk.
        update.full_id = "plugin.status.item_" + std::to_string((i * 2654435761u) % kItems);
        update.has_text = true;
        update.text = "busy";
        const bool ok =
            plugin::registry_interop::ApplyStatusItemUpdate(update, &order, &index);
        volatile bool sink = ok;
        (void)sink;
      }
    }
  });
}
#endif  // MICROIDE_HAS_LUA_PLUGINS

// ---- 102 / 019: SettingsOverlayService::RebuildSettingsRows ----------------
//
// Opening Settings / typing in the filter / a plugin reload rebuilds the row
// list and resolves each row's override layer. The fix indexes the user/project
// layers once (O(layer)) so resolution is O(1) per row, and builds per-category
// row indices so the render walk (RowAtVisibleIndex per row) is O(rows) not
// O(rows^2). This benches both: a full rebuild against a large plugin settings
// surface with dense overrides, then a category-by-category row walk.
void RunSettingsRowsRebuild(ScenarioContext& context) {
  constexpr int kSettings = 600;
  constexpr int kGroups = 24;
  std::vector<workspace::SettingInfo> settings;
  settings.reserve(kSettings);
  for (int i = 0; i < kSettings; ++i) {
    workspace::SettingInfo info;
    info.id = "perf.plugin.setting_" + std::to_string(i);
    info.label = "Setting number " + std::to_string(i);
    info.description = "Controls behavior variant " + std::to_string(i);
    info.type = workspace::SettingType::String;
    info.scope = (i % 3 == 0) ? workspace::SettingScope::User : workspace::SettingScope::Project;
    info.default_value = std::string("default");
    info.plugin_id = "perf.plugin";
    info.group = "Plugins \xE2\x86\x92 Group " + std::to_string(i % kGroups);
    settings.push_back(std::move(info));
  }
  // Dense override layers: index-once vs per-row linear Find is the fix.
  std::vector<std::pair<std::string, std::string>> user_settings;
  std::vector<std::pair<std::string, std::string>> project_settings;
  for (int i = 0; i < kSettings; i += 2) {
    user_settings.emplace_back("perf.plugin.setting_" + std::to_string(i), "user-override");
  }
  for (int i = 0; i < kSettings; i += 3) {
    project_settings.emplace_back("perf.plugin.setting_" + std::to_string(i), "project-override");
  }

  workspace::SettingsOverlayService service;
  context.Measure("settings_overlay.rebuild", [&]() {
    for (int iter = 0; iter < 100; ++iter) {
      service.RebuildSettingsRows(settings, user_settings, project_settings);
      volatile std::size_t sink = service.VisibleRowCount();
      (void)sink;
    }
  });

  // Category-by-category row walk -- the render loop that RowAtVisibleIndex made
  // O(rows) via category_row_indices_ instead of rescanning from row 0.
  service.RebuildSettingsRows(settings, user_settings, project_settings);
  const int category_count = static_cast<int>(service.Categories().size());
  context.Measure("settings_overlay.category_walk", [&]() {
    volatile std::size_t sink = 0;
    for (int iter = 0; iter < 200; ++iter) {
      for (int cat = 0; cat < category_count; ++cat) {
        const std::size_t rows = service.RowCountInCategory(cat);
        for (std::size_t r = 0; r < rows; ++r) {
          const auto* row = service.RowAtVisibleIndex(cat, static_cast<int>(r));
          if (row != nullptr) {
            sink += row->id.size();
          }
        }
      }
    }
    (void)sink;
  });
}

// ---- 024: util::ReadFileLineWindow bounded reference-snippet reader ---------
//
// Rendering a reference / definition result shows a small line window (the hit
// line +/- context) out of a possibly huge generated file. The fix reads a
// bounded window by streaming and stopping once last_line is reached, instead of
// materializing the whole file. This benches windows at scattered positions --
// weighted toward early lines, where the bounded read stops after a few KiB and
// a whole-file-read regression would stream megabytes.
void RunReferenceSnippetFileWindow(ScenarioContext& context) {
  std::error_code ec;
  const std::filesystem::path dir =
      std::filesystem::temp_directory_path(ec) / "microide_perf_line_window";
  std::filesystem::create_directories(dir, ec);
  const std::filesystem::path file = dir / "generated_huge.txt";
  {
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    for (int line = 1; line <= 40000; ++line) {
      out << "line " << line
          << ": generated content token alpha beta gamma delta epsilon zeta\n";
    }
  }
  // Representative reference-hit lines: mostly near the top (where the bounded
  // reader's early stop is the whole point), plus a couple deeper.
  const std::vector<std::size_t> hit_lines = {5, 40, 120, 400, 1200, 3500, 9000, 20000};

  context.Measure("reference_snippet.read_window", [&]() {
    volatile std::size_t sink = 0;
    for (int iter = 0; iter < 25; ++iter) {
      for (std::size_t hit : hit_lines) {
        const std::size_t first = hit > 1 ? hit - 1 : 1;
        std::vector<std::string> window = util::ReadFileLineWindow(file, first, hit + 1);
        sink += window.size();
      }
    }
    (void)sink;
  });

  std::filesystem::remove_all(dir, ec);
}

// ---- 054: multi-caret result-caret remap (fast + fallback paths) -----------
//
// Applying a multi-caret edit remaps every result caret across every lower edit.
// The fix folds the single-line-removed common case into one forward
// accumulator pass (O(carets)); multi-line ranges / anchored selections keep the
// exact O(carets^2) fallback. This benches both against many carets: the fast
// path at a scale where an accidental return to the per-caret inner loop is
// obvious, and the fallback at a smaller scale so its cost is pinned too.
void RunMultiCaretRemapBurst(ScenarioContext& context) {
  using editor::detail::ComputeReplacementShape;
  using editor::detail::MultiCaretRemapSite;
  using editor::detail::ResolveMultiCaretRemapSites;

  // Fast path: single-line zero-width inserts, no anchors. Many carets share a
  // handful of lines so the same-line column accumulation is exercised too.
  const auto build_fast_sites = [](int count) {
    std::vector<MultiCaretRemapSite> sites;
    sites.reserve(static_cast<std::size_t>(count));
    for (int i = 0; i < count; ++i) {
      MultiCaretRemapSite site;
      const std::size_t line = static_cast<std::size_t>(i / 8);
      const std::size_t col = static_cast<std::size_t>((i % 8) * 4);
      site.has_edit = true;
      site.removed.start = {line, col};
      site.removed.end = {line, col};
      site.shape = ComputeReplacementShape("x");
      site.landed = {line, col + 1};
      sites.push_back(site);
    }
    return sites;
  };
  // Fallback path: an anchored selection on every site forces the exact remap.
  const auto build_fallback_sites = [](int count) {
    std::vector<MultiCaretRemapSite> sites;
    sites.reserve(static_cast<std::size_t>(count));
    for (int i = 0; i < count; ++i) {
      MultiCaretRemapSite site;
      const std::size_t line = static_cast<std::size_t>(i / 8);
      const std::size_t col = static_cast<std::size_t>((i % 8) * 4);
      site.has_edit = true;
      site.removed.start = {line, col};
      site.removed.end = {line, col};
      site.shape = ComputeReplacementShape("yy");
      site.landed = {line, col + 2};
      site.anchor = editor::TextPosition{line, col};
      sites.push_back(site);
    }
    return sites;
  };

  const std::vector<MultiCaretRemapSite> fast_seed = build_fast_sites(4000);
  context.Measure("multi_caret.remap_fast", [&]() {
    for (int iter = 0; iter < 40; ++iter) {
      std::vector<MultiCaretRemapSite> sites = fast_seed;
      ResolveMultiCaretRemapSites(sites);
      volatile std::size_t sink = sites.size();
      (void)sink;
    }
  });

  const std::vector<MultiCaretRemapSite> fallback_seed = build_fallback_sites(1200);
  context.Measure("multi_caret.remap_fallback", [&]() {
    for (int iter = 0; iter < 20; ++iter) {
      std::vector<MultiCaretRemapSite> sites = fallback_seed;
      ResolveMultiCaretRemapSites(sites);
      volatile std::size_t sink = sites.size();
      (void)sink;
    }
  });
}

// ---- 060: snippet mirror-shift batching ------------------------------------
//
// Editing a linked snippet placeholder mirrors the text into every other
// occurrence and shifts the recorded ranges of all downstream placeholders. The
// old path called ShiftPlaceholdersAtOrAfter once per mirror (O(mirrors *
// placeholders)); the fix folds every mirror's delta into all ranges in one
// prefix-sum pass. This benches a snippet with many linked mirrors receiving
// successive mirrored keystrokes -- linear with the fix, quadratic without.
void RunSnippetManyMirrorEdit(ScenarioContext& context) {
  constexpr int kMirrors = 150;
  // "${1:x}" then " $1" * kMirrors: one primary placeholder plus kMirrors linked
  // occurrences of tab 1, then the final stop.
  std::string body = "${1:x}";
  body.reserve(body.size() + static_cast<std::size_t>(kMirrors) * 3 + 2);
  for (int i = 0; i < kMirrors; ++i) {
    body += " $1";
  }
  body += "$0";

  context.Measure("snippet.many_mirror_shift", [&]() {
    for (int iter = 0; iter < 30; ++iter) {
      editor::TextViewport viewport;
      viewport.LoadContent("--", "/tmp/perf_snippet.cpp");
      viewport.MoveCursorTo(0, 0);
      editor::SnippetSessionState session;
      viewport.BeginUndoGroup();
      const editor::SelectionRange trigger{{0, 0}, {0, 2}};
      if (!editor::ExpandSnippetAtSelection(viewport, session, trigger, body)) {
        return;
      }
      // Four successive mirrored keystrokes: each edits placeholder 1 and shifts
      // every downstream mirror's recorded range.
      for (int k = 0; k < 4; ++k) {
        editor::SnippetTryInsertText(viewport, session, "m");
      }
      volatile std::size_t sink = session.ranges_by_tab[1].size();
      (void)sink;
    }
  });
}

// ---- 058: user-config record decode (id->index map + disabled-id dedupe) ---
//
// Loading a project's user config decodes a settings layer and the disabled
// keybinding/plugin id lists. The fix builds an id->index map while decoding
// settings (so a duplicate-id override resolves in O(1), not a linear scan of
// the growing vector) and dedupes each disabled-id list through an unordered_set
// (AppendDisabledIdCapped) instead of a per-append linear membership scan. This
// benches decoding a large config: O(n) with the maps, O(n^2) without.
void RunUserConfigRecordDecode(ScenarioContext& context) {
  constexpr int kSettings = 2000;
  constexpr int kDisabled = 2000;
  workspace::PersistedUserConfigState state;
  state.ui_scale = 1.0f;
  state.settings.reserve(kSettings);
  for (int i = 0; i < kSettings; ++i) {
    state.settings.emplace_back("perf.setting_" + std::to_string(i),
                                "value_" + std::to_string(i));
  }
  state.disabled_keybinding_ids.reserve(kDisabled);
  state.disabled_plugin_ids.reserve(kDisabled);
  for (int i = 0; i < kDisabled; ++i) {
    state.disabled_keybinding_ids.push_back("cmd.disabled_" + std::to_string(i));
    state.disabled_plugin_ids.push_back("plugin.disabled_" + std::to_string(i));
  }

  std::vector<std::byte> encoded;
  if (!workspace::EncodeUserConfigRecord(state, &encoded)) {
    std::fprintf(stderr, "user_config_record_decode: encode failed; skipping\n");
    return;
  }

  context.Measure("persistence.user_config_decode", [&]() {
    for (int iter = 0; iter < 60; ++iter) {
      workspace::PersistedUserConfigState decoded;
      const bool ok = workspace::DecodeUserConfigRecord(encoded, &decoded);
      volatile std::size_t sink =
          decoded.settings.size() + decoded.disabled_keybinding_ids.size();
      (void)ok;
      (void)sink;
    }
  });
}

// ---- Registration ----------------------------------------------------------
//
// These are short-to-mid pure-unit micro-benchmarks (5-55 ms) whose ALLOCATION
// counts are exactly deterministic run-to-run but whose WALL times carry the
// software-render / xvfb scheduler jitter this reference runner exhibits on sub-
// 50 ms work (observed p50 wall swings of ~15-20% under background load, with
// identical allocations). The same tolerance percent gates both wall and
// allocations, so these widen the wall envelope (p50 25% / p95 45% / max 90%)
// while leaving the deterministic allocation gate more than tight enough to
// catch what these scenarios exist to catch: an accidental return to O(n^2),
// which blows both metrics up by hundreds of percent. A genuine constant-factor
// wall regression is caught precisely by the interleaved tools/perf-compare.py
// current-vs-main run, where shared machine load cancels out.
constexpr double kTdCoverageTolP50 = 25.0;
constexpr double kTdCoverageTolP95 = 45.0;
constexpr double kTdCoverageTolMax = 90.0;

const ScenarioRegistration g_perf_assist_ranked_union_merge({Scenario{
    .name = "assist_ranked_union_merge",
    .smoke = true,
    .baseline_gated = true,
    .tolerance_p50_percent = kTdCoverageTolP50,
    .tolerance_p95_percent = kTdCoverageTolP95,
    .tolerance_max_percent = kTdCoverageTolMax,
    .run = RunAssistRankedUnionMerge,
}});
const ScenarioRegistration g_perf_review_comments_registry_lookup({Scenario{
    .name = "review_comments_registry_lookup",
    .smoke = true,
    .baseline_gated = true,
    .tolerance_p50_percent = kTdCoverageTolP50,
    .tolerance_p95_percent = kTdCoverageTolP95,
    .tolerance_max_percent = kTdCoverageTolMax,
    .run = RunReviewCommentsRegistryLookup,
}});
#if MICROIDE_HAS_LUA_PLUGINS
const ScenarioRegistration g_perf_plugin_status_item_update({Scenario{
    .name = "plugin_status_item_update",
    .smoke = true,
    .baseline_gated = true,
    .tolerance_p50_percent = kTdCoverageTolP50,
    .tolerance_p95_percent = kTdCoverageTolP95,
    .tolerance_max_percent = kTdCoverageTolMax,
    .run = RunPluginStatusItemUpdate,
}});
#endif
const ScenarioRegistration g_perf_settings_rows_rebuild({Scenario{
    .name = "settings_rows_rebuild",
    .smoke = true,
    .baseline_gated = true,
    .tolerance_p50_percent = kTdCoverageTolP50,
    .tolerance_p95_percent = kTdCoverageTolP95,
    .tolerance_max_percent = kTdCoverageTolMax,
    .run = RunSettingsRowsRebuild,
}});
const ScenarioRegistration g_perf_reference_snippet_file_window({Scenario{
    .name = "reference_snippet_file_window",
    .smoke = true,
    .baseline_gated = true,
    .tolerance_p50_percent = kTdCoverageTolP50,
    .tolerance_p95_percent = kTdCoverageTolP95,
    .tolerance_max_percent = kTdCoverageTolMax,
    .run = RunReferenceSnippetFileWindow,
}});
const ScenarioRegistration g_perf_multi_caret_remap_burst({Scenario{
    .name = "multi_caret_remap_burst",
    .smoke = true,
    .baseline_gated = true,
    .tolerance_p50_percent = kTdCoverageTolP50,
    .tolerance_p95_percent = kTdCoverageTolP95,
    .tolerance_max_percent = kTdCoverageTolMax,
    .run = RunMultiCaretRemapBurst,
}});
const ScenarioRegistration g_perf_snippet_many_mirror_edit({Scenario{
    .name = "snippet_many_mirror_edit",
    .smoke = true,
    .baseline_gated = true,
    .tolerance_p50_percent = kTdCoverageTolP50,
    .tolerance_p95_percent = kTdCoverageTolP95,
    .tolerance_max_percent = kTdCoverageTolMax,
    .run = RunSnippetManyMirrorEdit,
}});
const ScenarioRegistration g_perf_user_config_record_decode({Scenario{
    .name = "user_config_record_decode",
    .smoke = true,
    .baseline_gated = true,
    .tolerance_p50_percent = kTdCoverageTolP50,
    .tolerance_p95_percent = kTdCoverageTolP95,
    .tolerance_max_percent = kTdCoverageTolMax,
    .run = RunUserConfigRecordDecode,
}});

}  // namespace
}  // namespace microide::tests::perf
