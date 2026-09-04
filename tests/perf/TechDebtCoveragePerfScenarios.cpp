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
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "compare/BranchReviewStateService.h"
#include "compare/BranchReviewStateTypes.h"
#include "compare/CompareModel.h"
#include "compare/CompareReviewTypes.h"
#include "editor/SnippetEngine.h"
#include "editor/TextViewport.h"
#include "editor/TextViewportInternal.h"
#include "platform/Filesystem.h"
#include "plugin/PluginHost.h"
#include "plugin/PluginRegistryInterop.h"
#include "project/ProjectTraversalFilter.h"
#include "util/PerformanceCounters.h"
#include "util/TextFileIO.h"
#include "workspace/AssistProviderMerge.h"
#include "workspace/services/SettingsOverlayService.h"
#include "workspace/git/CompareTabReview.h"
#include "workspace/persistence/WorkspacePersistenceFormat.h"
#include "workspace/registries/WorkspaceSettingsRegistry.h"
#include "workspace/state/WorkspaceTabState.h"

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

  // Built OUTSIDE the measured window. Composing `"plugin.status.item_" +
  // std::to_string(...)` per call was two allocations per update against a lookup
  // that makes none, so 95% of this phase's 168,001 allocations were the scenario
  // building its own input — a gate on operator+ wearing ApplyStatusItemUpdate's
  // name, which a real regression in the lookup could not have moved (TD-2026-08-06-159).
  std::vector<plugin::registry_interop::StatusItemUpdate> updates;
  updates.reserve(kItems);
  for (int i = 0; i < kItems; ++i) {
    plugin::registry_interop::StatusItemUpdate update;
    // Scatter the target so a linear-scan regression pays the full walk.
    update.full_id = "plugin.status.item_" + std::to_string((i * 2654435761u) % kItems);
    update.has_text = true;
    update.text = "busy";
    updates.push_back(std::move(update));
  }

  context.Measure("status_registry.apply_update", [&]() {
    for (int iter = 0; iter < 40; ++iter) {
      for (const plugin::registry_interop::StatusItemUpdate& update : updates) {
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

// ---- snippet expansion: parse + insert of a language-server body -----------
//
// The body shapes clangd / rust-analyzer / typescript-language-server send:
// nested placeholders, a choice, variables (one set, one empty with a default,
// one unknown), a transform to step over, and escapes. ExpandSnippetAtSelection
// parses through the editor's variable resolver and records the nested links,
// so this is the whole per-completion cost the user pays on accepting a snippet
// item -- the mirror-edit scenario above measures only the keystrokes after.
void RunSnippetExpandCompletion(ScenarioContext& context) {
  static constexpr std::string_view kBody =
      "template <typename ${1:T}>\n"
      "${2|inline,constexpr,static|} ${3:auto} ${4:${TM_FILENAME_BASE}_fn}("
      "${5:const ${1:T}\\& value}) {\n"
      "\t// $LINE_COMMENT ${TM_SELECTED_TEXT:body} \\$1 \\}\n"
      "\t${6/(.*)/${1:/upcase}/}${UNKNOWN_VAR}\n"
      "\treturn ${7:value};\n"
      "}$0";
  context.Measure("snippet.expand_completion", [&]() {
    for (int iter = 0; iter < 40; ++iter) {
      editor::TextViewport viewport;
      viewport.LoadContent("int x;\nfoo", "/tmp/perf_snippet_expand.cpp");
      viewport.MoveCursorTo(1, 3);
      editor::SnippetSessionState session;
      viewport.BeginUndoGroup();
      const editor::SelectionRange trigger{{1, 0}, {1, 3}};
      if (!editor::ExpandSnippetAtSelection(viewport, session, trigger, kBody)) {
        return;
      }
      volatile std::size_t sink = session.ranges_by_tab.size() + session.nested_links.size();
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

// ---- 062: ApplyBranchReviewPresentationMarkers per-hunk memo ---------------
//
// In branch-review mode every visible compare row gets a review marker/note
// resolved from the branch-review state. Hunk status is constant per hunk but a
// large diff has many rows per hunk, so the fix memoizes (marker, has_note) per
// hunk index -- each hunk's HunkStatus/HasNote scan (over the target's reviewed-
// hunk list) runs once instead of once per row. This benches a diff with many
// rows across many hunks against a review service holding a large reviewed-hunk
// list, so each status scan is non-trivial: O(hunks * list) with the memo,
// O(rows * list) without.
void RunBranchReviewPresentationMarkers(ScenarioContext& context) {
  // Two texts differing every 8th line: ~50 hunks over ~400 rows. Kept small
  // because a single marker pass over the diff is inherently allocation-heavy
  // per row (each row recomposes its display summary); the memo's win is the
  // rows/hunks ratio, which this scale still exercises.
  std::string left;
  std::string right;
  constexpr int kLines = 400;
  for (int i = 0; i < kLines; ++i) {
    const std::string n = std::to_string(i);
    left += "line " + n + " original content token here\n";
    if (i % 8 == 0) {
      right += "line " + n + " MODIFIED content token here\n";
    } else {
      right += "line " + n + " original content token here\n";
    }
  }

  workspace::CompareTabState tab;
  tab.path = "src/reviewed_file.cpp";
  tab.review_mode = compare::CompareReviewMode::Branch;
  tab.branch_target.repository_root = "/repo";
  tab.branch_target.base_commit = "aaaaaaaaaaaa";
  tab.branch_target.head_commit = "bbbbbbbbbbbb";
  tab.branch_target.merge_base_commit = "cccccccccccc";
  tab.branch_target.snapshot_generation = 1;
  tab.model = compare::BuildCompareModel(left, right);
  workspace::RefreshCompareTabPresentation(tab);

  // Populate the review service with a large reviewed-hunk list + notes so each
  // HunkStatus / HasNote resolution does a real scan (bounded by the per-target
  // hunk cap). The identities need not match the model's hunks -- a miss scans
  // the whole list, which is the worst case the memo is protecting.
  compare::BranchReviewStateService service;
  for (int i = 0; i < 200; ++i) {
    compare::BranchReviewHunkIdentity identity;
    identity.path = "src/reviewed_file.cpp";
    identity.old_start = i * 3;
    identity.old_count = 2;
    identity.new_start = i * 3;
    identity.new_count = 3;
    identity.content_hash = static_cast<std::uint64_t>(i) * 2654435761u;
    service.MarkHunkReviewed(tab.branch_target, identity);
  }
  for (int i = 0; i < 40; ++i) {
    compare::BranchReviewHunkIdentity identity;
    identity.path = "src/reviewed_file.cpp";
    identity.old_start = i * 5;
    identity.new_start = i * 5;
    identity.new_count = 1;
    service.SetNote(tab.branch_target, compare::BranchReviewNoteScope::Hunk,
                    "src/reviewed_file.cpp", identity, "needs another look");
  }

  context.Measure("branch_review.presentation_markers", [&]() {
    for (int iter = 0; iter < 12; ++iter) {
      // The pass skips when none of its inputs moved. Bump the presentation
      // revision so each iteration measures a real resolve — that is what the
      // phase's name claims, and a phase that silently became 11 no-ops and one
      // resolve would keep passing its gate while measuring nothing.
      ++tab.presentation_revision;
      workspace::ApplyBranchReviewPresentationMarkers(tab, service);
      volatile std::size_t sink = tab.presentation.rows.size();
      (void)sink;
    }
  });

  // The other half: the mouse-move case, where nothing moved. This is what the
  // derived-state refresh actually does most of the time, and it must stay O(1).
  context.Measure("branch_review.presentation_markers_unchanged", [&]() {
    for (int iter = 0; iter < 12; ++iter) {
      workspace::ApplyBranchReviewPresentationMarkers(tab, service);
      volatile std::size_t sink = tab.presentation.rows.size();
      (void)sink;
    }
  });
}

// ---- 174: ProjectTraversalFilter::Includes --------------------------------
//
// Every whole-tree walk in the app funnels through this one call, once per
// filesystem entry: the initial file-index build (documented in FileIndexWatcher
// as "the dominant single cost of opening a project"), the poll-fallback
// re-walk, the inotify registration, the sidebar tree and the project scanner.
// A project with 20,000 entries calls it 20,000 times per walk.
//
// TD-2026-08-10-174 is about `lexically_normal()` on paths that are already
// normal — ~12 allocations for a no-op — and this is the hottest instance of it
// in the tree. The scan below is pure path arithmetic: the per-directory matcher
// cache is warmed before the measured phase, so the phase measures the filter's
// own per-entry cost and nothing else. Allocation count is the oracle; a
// re-added `lexically_normal()` on the entry path shows up as +12 per entry.
void RunProjectTraversalFilterScan(ScenarioContext& context) {
  // A root that does not exist on disk, deliberately: the only filesystem work
  // Includes() does is IgnoreMatcher::LoadIgnoreFile per NEW directory, which the
  // warm pass below absorbs into setup. The measured phase then touches no
  // syscalls at all and is deterministic on any host.
  const std::filesystem::path root("/microide-perf/traversal-root");
  project::ProjectTraversalFilter filter(root, {});

  // Shaped like a source tree: a few dozen directories, most entries at depth
  // 3-4, a handful under an ignored build directory so the reject path is
  // measured too.
  // The entry kind is precomputed rather than derived per call: `path::extension()`
  // builds a path and would put the scenario's own allocations inside the phase.
  //
  // Built ONCE for the process, not once per iteration. It is scenario INPUT, and
  // 2,048 `std::filesystem::path`s cost ~14,000 allocations to construct — in a
  // scenario whose measured phase allocates zero. That made every one of the
  // scenario's iteration-level numbers a measurement of its own fixture:
  // `p50_net_heap_bytes` scaled 1:1 with the entry count (doubling the tree took
  // it from 67,680 to 135,056), so the retention gate was reporting how big the
  // fixture is, not what the filter retains (TD-2026-08-12-191). Same rule as
  // TD-2026-08-07-163: build scenario inputs outside the measured window.
  static const std::vector<std::pair<std::filesystem::path, platform::PathType>> entries = [&] {
    std::vector<std::pair<std::filesystem::path, platform::PathType>> built;
    built.reserve(2048);
    for (int dir = 0; dir < 32; ++dir) {
      const std::string top = dir % 8 == 0 ? "build" : "src" + std::to_string(dir);
      for (int sub = 0; sub < 4; ++sub) {
        const std::string subdir = top + "/module" + std::to_string(sub);
        built.emplace_back(root / subdir, platform::PathType::Directory);
        for (int file = 0; file < 15; ++file) {
          built.emplace_back(root / (subdir + "/unit" + std::to_string(file) + ".cpp"),
                             platform::PathType::RegularFile);
        }
      }
    }
    return built;
  }();

  // `entries` is static, so it is named directly rather than captured.
  const auto scan = [&filter] {
    std::size_t kept = 0;
    for (const auto& [entry, type] : entries) {
      kept += filter.Includes(entry, type) ? 1u : 0u;
    }
    return kept;
  };

  // Warm the per-directory matcher cache (and the failed .gitignore opens behind
  // it) so the measured phase is the filter's steady-state per-entry cost.
  volatile std::size_t warm_sink = scan();
  (void)warm_sink;

  context.Measure("project.traversal_filter_scan", [&]() {
    volatile std::size_t sink = scan();
    (void)sink;
  });
}

// The scenario above measures 0.34 us per entry. The same filter over this
// repository measures 1.6 us per entry, and measured 6.9 us before
// TD-2026-08-17-257's fix — so the gate that was cited as clearing the filter of
// that entry's 126 ms walk was structurally incapable of seeing it. The
// difference is the fixture: a shallow tree with rules that match almost nothing
// exercises the reject-early path and nothing else.
//
// This is the other half. A real project walk is mostly entries that are
// INDIVIDUALLY ignored (this repo visits ~26,000 to keep 8,000), sitting several
// directories deep under parents that are not themselves ignored, matched against
// a rule set of ~45 patterns of every shape. Both scenarios are kept: the shallow
// one is the floor, this one is the workload.
//
// The three counters are the deterministic part, and they gate below as hard
// invariants rather than as baseline numbers, because what matters is a RATIO
// that a wall time cannot express: how many times the rule set runs per entry
// asked about, and whether the ancestor chain is walked per directory or per file.
void RunProjectTraversalFilterDeepTree(ScenarioContext& context) {
  // Not on disk, for the same reason the scenario above is not: the measured
  // phase must touch no syscalls. Rules arrive through the exclude-glob list,
  // which parses them exactly as a root `.gitignore` would.
  const std::filesystem::path root("/microide-perf/traversal-deep-root");
  const std::vector<std::string> excludes = {
      "*.o",      "*.pyc",     "*.swp",       "*~",        "*.orig",   "*.log",
      "*.tmp",    "*.class",   ".DS_Store",   "build/",    "builds/",  "out/",
      "dist/",    "target/",   "bin/",        "obj/",      "vendor/",  "coverage/",
      "node_modules/",         "__pycache__/", ".cache/",  ".venv/",
      "cmake-build-*/",        "/result",     "/result-*", "generated/*/*",
      "**/fixtures/*",         "docs/build/", "!keep.log",
  };
  project::ProjectTraversalFilter filter(root, excludes);

  // Built once for the process — scenario INPUT, not measured work
  // (TD-2026-08-07-163). Files sit at depth 4 under 128 leaf directories, which is
  // where a real source tree's mass is, and the name rotation makes roughly two
  // thirds of them individually ignored by a suffix rule while their parents stay
  // included. Two of the mid-level names are themselves ignored (`vendor`,
  // `build`), which is what puts entries on the excluded-ancestor path.
  struct DeepTree {
    std::vector<std::pair<std::filesystem::path, platform::PathType>> entries;
    std::size_t distinct_parents = 0;
  };
  static const DeepTree tree = [&] {
    DeepTree built;
    built.entries.reserve(2048);
    static constexpr std::string_view kMidNames[] = {"core", "vendor", "render", "build"};
    static constexpr std::string_view kFileSuffixes[] = {".cpp", ".o", ".h", ".pyc",
                                                         ".log", ".ts", ".swp", ".md"};
    std::set<std::string> parents;
    for (int pkg = 0; pkg < 8; ++pkg) {
      for (const std::string_view mid : kMidNames) {
        for (int leaf = 0; leaf < 4; ++leaf) {
          const std::string directory = "pkg" + std::to_string(pkg) + "/" + std::string(mid) +
                                        "/leaf" + std::to_string(leaf);
          built.entries.emplace_back(root / directory, platform::PathType::Directory);
          parents.insert((root / directory).parent_path().native());
          for (int file = 0; file < 15; ++file) {
            const std::string name = "unit" + std::to_string(file) +
                                     std::string(kFileSuffixes[static_cast<std::size_t>(file) %
                                                               std::size(kFileSuffixes)]);
            built.entries.emplace_back(root / (directory + "/" + name),
                                       platform::PathType::RegularFile);
            parents.insert((root / directory).native());
          }
        }
      }
    }
    built.distinct_parents = parents.size();
    return built;
  }();

  const auto scan_with = [](project::ProjectTraversalFilter& target) {
    std::size_t kept = 0;
    for (const auto& [entry, type] : tree.entries) {
      kept += target.Includes(entry, type) ? 1u : 0u;
    }
    return kept;
  };
  const auto scan = [&] { return scan_with(filter); };

  // Warm the per-directory caches so the measured phase is steady-state per-entry
  // cost, exactly as the shallow scenario does.
  const std::size_t warm_kept = scan();

  // One instrumented pass OUTSIDE the measured window, so the invariants below
  // read one scan's worth of counters whatever --iterations says. It runs on a
  // FRESH filter on purpose: the per-directory memo is what is being pinned, and
  // a warmed filter walks no ancestor chain at all, which would make the bound
  // below vacuously true.
  project::ProjectTraversalFilter cold_filter(root, excludes);
  const std::uint64_t queries_before =
      util::ReadPerformanceCounter(util::PerfCounterId::ProjectIgnoreFilterQueries);
  const std::uint64_t evaluations_before =
      util::ReadPerformanceCounter(util::PerfCounterId::ProjectIgnoreFilterRuleSetEvaluations);
  const std::uint64_t ancestor_before =
      util::ReadPerformanceCounter(util::PerfCounterId::ProjectIgnoreFilterAncestorScans);
  const std::size_t instrumented_kept = scan_with(cold_filter);
  const std::uint64_t queries =
      util::ReadPerformanceCounter(util::PerfCounterId::ProjectIgnoreFilterQueries) -
      queries_before;
  const std::uint64_t evaluations =
      util::ReadPerformanceCounter(util::PerfCounterId::ProjectIgnoreFilterRuleSetEvaluations) -
      evaluations_before;
  const std::uint64_t ancestor_scans =
      util::ReadPerformanceCounter(util::PerfCounterId::ProjectIgnoreFilterAncestorScans) -
      ancestor_before;

  context.Measure("project.traversal_filter_deep_tree", [&]() {
    volatile std::size_t sink = scan();
    (void)sink;
  });

  // --- Hard invariants -------------------------------------------------------
  //
  // Vacuity first: a filter that answered the same way for everything would sail
  // past the ratio checks below while measuring nothing.
  if (instrumented_kept != warm_kept || instrumented_kept == 0 ||
      instrumented_kept >= tree.entries.size()) {
    throw std::runtime_error(
        "project_traversal_filter_deep_tree: the fixture kept " +
        std::to_string(instrumented_kept) + " of " + std::to_string(tree.entries.size()) +
        " entries (warm pass kept " + std::to_string(warm_kept) +
        "); the tree must exercise BOTH verdicts and be stable across passes");
  }
  if (queries != tree.entries.size()) {
    throw std::runtime_error("project_traversal_filter_deep_tree: " + std::to_string(queries) +
                             " filter queries for " + std::to_string(tree.entries.size()) +
                             " entries; the counter is not measuring this scan alone");
  }
  // One matcher layer per query is the floor here (the root matcher; no nested
  // .gitignore exists on this synthetic tree). The bound is 2x that, which still
  // fails the shape TD-2026-08-17-257 fixed: re-checking the ancestor chain per
  // entry read 4-5 evaluations per query on a depth-4 tree.
  if (evaluations > 2 * queries) {
    throw std::runtime_error(
        "project_traversal_filter_deep_tree: the rule set ran " + std::to_string(evaluations) +
        " times for " + std::to_string(queries) +
        " queries; it must run a bounded number of times per entry, not once per path component");
  }
  // The excluded-ancestor answer is a property of the parent DIRECTORY. Walking
  // it per entry is the regression; this bound is ~13x below what that reads.
  if (ancestor_scans > tree.distinct_parents) {
    throw std::runtime_error("project_traversal_filter_deep_tree: walked the ancestor chain " +
                             std::to_string(ancestor_scans) + " times for " +
                             std::to_string(tree.distinct_parents) +
                             " distinct parent directories; the answer is cached per directory");
  }
  if (ancestor_scans == 0) {
    throw std::runtime_error(
        "project_traversal_filter_deep_tree: the ancestor chain was never walked, so the "
        "excluded-ancestor path is not being measured at all");
  }
}

// ---- Registration ----------------------------------------------------------
//
// These are short-to-mid pure-unit micro-benchmarks (20-55 ms). Their ALLOCATION
// counts are exactly deterministic run-to-run -- the true complexity oracle --
// so those envelopes stay tight (10/20/50%): a return to O(n^2) blows the
// allocation count up by hundreds to thousands of percent and is caught
// precisely. Their WALL times, by contrast, carry the software-render / xvfb
// scheduler jitter this shared reference runner exhibits on sub-50 ms work: the
// median swings up to ~2x with ambient machine load and the 10-sample p95/max
// tail is dominated by single context-switch outliers. Gating wall on the tight
// allocation percent would false-trip constantly, so the wall envelopes are
// widened (p50 75% / p95 250% / max 400%, in the spirit of the
// editor_moby_dick_workout precedent) -- still far below an O(n^2) blowup, and a
// genuine constant-factor wall regression is caught precisely by the interleaved
// tools/perf-compare.py current-vs-main run, where shared machine load cancels.
// Wall and allocation tolerances are decoupled per baseline (see Baseline.h), so
// the loose wall envelope never blinds the tight allocation complexity gate.

const ScenarioRegistration g_perf_assist_ranked_union_merge({Scenario{
    .name = "assist_ranked_union_merge",
    .smoke = true,
    .baseline_gated = true,
    .tolerance_p95_percent = tolerance::kJitterWallP95,
    .tolerance_max_percent = tolerance::kJitterWallMax,
    .run = RunAssistRankedUnionMerge,
}});
#if MICROIDE_HAS_LUA_PLUGINS
const ScenarioRegistration g_perf_plugin_status_item_update({Scenario{
    .name = "plugin_status_item_update",
    .smoke = true,
    .baseline_gated = true,
    .tolerance_p95_percent = tolerance::kJitterWallP95,
    .tolerance_max_percent = tolerance::kJitterWallMax,
    .run = RunPluginStatusItemUpdate,
}});
#endif
const ScenarioRegistration g_perf_settings_rows_rebuild({Scenario{
    .name = "settings_rows_rebuild",
    .smoke = true,
    .baseline_gated = true,
    .tolerance_p95_percent = tolerance::kJitterWallP95,
    .tolerance_max_percent = tolerance::kJitterWallMax,
    .run = RunSettingsRowsRebuild,
}});
const ScenarioRegistration g_perf_reference_snippet_file_window({Scenario{
    .name = "reference_snippet_file_window",
    .smoke = true,
    .baseline_gated = true,
    .tolerance_p95_percent = tolerance::kJitterWallP95,
    .tolerance_max_percent = tolerance::kJitterWallMax,
    .run = RunReferenceSnippetFileWindow,
}});
const ScenarioRegistration g_perf_multi_caret_remap_burst({Scenario{
    .name = "multi_caret_remap_burst",
    .smoke = true,
    .baseline_gated = true,
    .tolerance_p95_percent = tolerance::kJitterWallP95,
    .tolerance_max_percent = tolerance::kJitterWallMax,
    .run = RunMultiCaretRemapBurst,
}});
const ScenarioRegistration g_perf_snippet_many_mirror_edit({Scenario{
    .name = "snippet_many_mirror_edit",
    .smoke = true,
    .baseline_gated = true,
    .tolerance_p95_percent = tolerance::kJitterWallP95,
    .tolerance_max_percent = tolerance::kJitterWallMax,
    .run = RunSnippetManyMirrorEdit,
}});
const ScenarioRegistration g_perf_snippet_expand_completion({Scenario{
    .name = "snippet_expand_completion",
    .smoke = true,
    .baseline_gated = true,
    .tolerance_p95_percent = tolerance::kJitterWallP95,
    .tolerance_max_percent = tolerance::kJitterWallMax,
    .run = RunSnippetExpandCompletion,
}});
const ScenarioRegistration g_perf_user_config_record_decode({Scenario{
    .name = "user_config_record_decode",
    .smoke = true,
    .baseline_gated = true,
    .tolerance_p95_percent = tolerance::kJitterWallP95,
    .tolerance_max_percent = tolerance::kJitterWallMax,
    .run = RunUserConfigRecordDecode,
}});
const ScenarioRegistration g_perf_branch_review_presentation_markers({Scenario{
    .name = "branch_review_presentation_markers",
    .smoke = true,
    .baseline_gated = true,
    .tolerance_p95_percent = tolerance::kJitterWallP95,
    .tolerance_max_percent = tolerance::kJitterWallMax,
    .run = RunBranchReviewPresentationMarkers,
}});
const ScenarioRegistration g_perf_project_traversal_filter_scan({Scenario{
    .name = "project_traversal_filter_scan",
    .smoke = true,
    .baseline_gated = true,
    // Iteration 0 builds the process-wide entry fixture (see the scenario body);
    // every iteration after it measures the filter alone. Declared rather than
    // absorbed into the p50, which is what the warmup mechanism is for.
    .warmup_iterations = 1,
    .tolerance_p95_percent = tolerance::kJitterWallP95,
    .tolerance_max_percent = tolerance::kJitterWallMax,
    // Bumped with the fixture move: the old numbers are dominated by per-iteration
    // fixture construction and describe a different measurement
    // (TD-2026-08-12-191, TD-2026-08-07-167).
    .measurement_revision = 2,
    .run = RunProjectTraversalFilterScan,
}});
const ScenarioRegistration g_perf_project_traversal_filter_deep_tree({Scenario{
    .name = "project_traversal_filter_deep_tree",
    .smoke = true,
    .baseline_gated = true,
    // Same reason as the scenario above: iteration 0 builds the process-wide entry
    // fixture, and the warm + instrumented passes fill the per-directory caches.
    .warmup_iterations = 1,
    .tolerance_p95_percent = tolerance::kJitterWallP95,
    .tolerance_max_percent = tolerance::kJitterWallMax,
    .run = RunProjectTraversalFilterDeepTree,
}});


// TD-2026-08-14-212: the tab drag had no scenario at all. The 2026-08-14 pass
// moved it off the hover pipeline and off the full-window repaint and measured
// the win with a throwaway in-test harness, which is exactly the shape that
// leaves a hot path ungated once the harness is deleted.
//
// Three things are pinned here, matching what that entry asked for:
//
//  1. the per-motion-event cost of `TabMouseCoordinator::HandleMotion` on an
//     OVERFLOWING strip (the interesting case: it resolves the strip, re-lays the
//     visible list on an auto-scroll step, and reseeds the slide targets) --
//     `tab_drag.motion_burst`, a declared phase so its allocations gate
//     separately from the 40-tab setup that dwarfs them;
//  2. the allocation count of one drag from press to drop, which should be
//     bounded by the slide-target vectors (seeded on a slot CHANGE) and nothing
//     per event -- the same phase, whose count must scale with slots crossed
//     rather than with events sent;
//  3. the damage AREA per drag frame, which a headless event-cost measurement
//     cannot see at all and is the change with the largest real-world effect.
//     `MouseMoveDragging` hands back the shell's own invalidation for that one
//     event, so the scenario adds the rect areas up itself and THROWS if the
//     drag ever asks for a full-window repaint or spends more than a few strip
//     areas per event. That check is a hard invariant rather than a baseline
//     number: it is deterministic, it is independent of the runner's clock, and
//     it fails loudly the moment somebody reverts the drag to `RequestFullRedraw`
//     -- which is precisely the regression a wall-time gate would report as a
//     rounding error.
void RunEditorTabDragBurst(ScenarioContext& context) {
  const std::filesystem::path project = "tests/perf/fixtures/settings_tabs_project";
  if (!RequireFixture(context, project, "editor_tab_drag_burst")) {
    return;
  }
  (void)context.Open(project);
  context.PumpFrames(2);

  // 40 tabs at 1920 logical pixels is a comfortably overflowing strip, which is
  // the case worth measuring: the drop slot pins to what is VISIBLE and the
  // pointer parked at an edge walks the strip under itself.
  constexpr int kTabCount = 40;
  int opened = 0;
  for (int index = 1; index <= kTabCount; ++index) {
    char name[32];
    std::snprintf(name, sizeof(name), "unit_%02d.cpp", index);
    const std::filesystem::path file = project / "src" / name;
    if (!PathExistsNoThrow(file)) {
      continue;
    }
    context.OpenTab(file);
    ++opened;
  }
  if (opened < 8) {
    context.SkipScenario("editor_tab_drag_burst: fixture opened only " + std::to_string(opened) +
                         " tabs under " + (project / "src").string());
    return;
  }
  context.PumpFrames(2);

  const SDL_FRect strip = context.EditorGroupTabStripRect(0);
  // The FIRST VISIBLE tab, not tab 0 and not the last one: an overflowing strip
  // is a window onto the list, so most model indices have no rect at all, and
  // asking for one that is scrolled off measures a press on empty chrome.
  SDL_FRect source{};
  for (int index = 0; index < opened && source.w <= 0.0f; ++index) {
    source = context.EditorTabRect(static_cast<std::size_t>(index));
  }
  if (strip.w <= 0.0f || source.w <= 0.0f) {
    context.SkipScenario("editor_tab_drag_burst: the tab strip has no geometry to drag across");
    return;
  }
  const float strip_area = strip.w * strip.h;
  const float grab_x = source.x + source.w * 0.5f;
  const float grab_y = source.y + source.h * 0.5f;

  // Enough events that per-event cost dominates the fixed press/release, swept
  // back and forth across the whole strip so the burst crosses many slots and
  // parks at both edges (which is what arms the auto-scroll).
  constexpr int kMotionEvents = 600;
  double damage_pixels = 0.0;
  int full_redraws = 0;

  context.MousePress(grab_x, grab_y);
  context.Measure("tab_drag.motion_burst", [&] {
    for (int i = 0; i < kMotionEvents; ++i) {
      // A triangle sweep: 0 -> 1 -> 0 across the strip's usable width.
      const int period = kMotionEvents / 4;
      const int phase = i % (2 * period);
      const float t = phase < period
                          ? static_cast<float>(phase) / static_cast<float>(period)
                          : 2.0f - static_cast<float>(phase) / static_cast<float>(period);
      const float x = strip.x + t * (strip.w - 1.0f);
      const workspace::WorkspaceShell::RenderInvalidation redraw =
          context.MouseMoveDragging(x, grab_y);
      if (redraw.full) {
        ++full_redraws;
      }
      for (const SDL_FRect& rect : redraw.rects) {
        damage_pixels += static_cast<double>(rect.w) * static_cast<double>(rect.h);
      }
    }
  });
  context.Measure("tab_drag.drop",
                  [&] { context.MouseRelease(strip.x + strip.w * 0.5f, grab_y); });

  // Hard invariants on repaint scope. `strip_area` is ~40x the height of one
  // tab strip band; a drag that damages the window instead would read ~60x this
  // per event on a 1920x1080 surface.
  if (full_redraws != 0) {
    throw std::runtime_error("editor_tab_drag_burst: the drag asked for " +
                             std::to_string(full_redraws) +
                             " FULL-WINDOW repaints; a tab drag must damage the strip it is on");
  }
  const double average_areas_per_event =
      damage_pixels / (static_cast<double>(kMotionEvents) * static_cast<double>(strip_area));
  // Measured 1.09 strip areas per event on the reference runner (the strip plus
  // its ghost-shadow padding). 2.0 leaves room for the second strip a split adds
  // and for the frame that damages the tooltip the gesture retired, while still
  // failing an order of magnitude below a full-window repaint — a 1920x1080
  // window is ~38 strip areas, so that regression reads as 38.0 here. The bound
  // was probed by lowering it until it fired, which is the only way to know a
  // check like this is not structurally green (dev-docs/project/validation-traps.md).
  if (average_areas_per_event > 2.0) {
    throw std::runtime_error(
        "editor_tab_drag_burst: the drag damaged " + std::to_string(average_areas_per_event) +
        " tab-strip areas per motion event; the drag repaint must stay scoped to the strip");
  }
  // Vacuity guard: a drag that damaged NOTHING would sail past the two checks
  // above while rendering no feedback at all.
  if (damage_pixels <= 0.0) {
    throw std::runtime_error(
        "editor_tab_drag_burst: the drag produced no damage at all, so it measured nothing");
  }
}

const ScenarioRegistration g_perf_editor_tab_drag_burst({Scenario{
    .name = "editor_tab_drag_burst",
    .smoke = false,
    .baseline_gated = true,
    // warmup: the first pass pays the project's cold open (background file-index
    // build, initial watch batch) plus every tab's first title/width measurement,
    // which dwarfs the measured burst and would otherwise govern p95/max on its
    // own -- the same shape as settings_change_many_tabs.
    .warmup_iterations = 1,
    // The TIMING half of this baseline is deliberately advisory (the record
    // carries `timing_is_advisory`), and it is not a shortcut: the wall envelope
    // is derived from the recording run's own spread and floored at 25 %, so a
    // declared tolerance cannot widen it — `EffectiveWallTolerance` takes the
    // MIN. This scenario's iteration is ~5 ms and ~80 % of it is opening 40 tabs,
    // so its `max_wall_ms` rides that envelope at 82-93 % across consecutive
    // standalone runs and exceeds it in a full-suite run, with `p50_allocations`
    // byte-identical (14,167.5) every single time. That is the lane, not the
    // code. TD-2026-08-12-186's precedent applies: arm the deterministic half,
    // report the machine-sensitive half and say so.
    //
    // Nothing is lost by it. What this scenario exists to gate is the two phase
    // allocation counts and the three repaint-scope invariants asserted in the
    // body, and all four are deterministic and clock-independent.
    //
    // Allocations are the oracle here and they are deterministic: the burst
    // reads 190/191 across ten iterations (0.5 % spread), because the seed path
    // runs on a slot CHANGE and nothing else allocates per event. 3 % is 6x that
    // spread and still fails on a single allocation added per motion event, which
    // would be +600 on a 190-allocation phase. Inherited by the per-phase gate,
    // which is the one that matters — the scenario total is dominated by the
    // 40-tab setup.
    .tolerance_alloc_p50_percent = 3.0,
    .run = RunEditorTabDragBurst,
}});

}  // namespace
}  // namespace microide::tests::perf
