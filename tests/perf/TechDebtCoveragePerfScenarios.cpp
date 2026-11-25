#include "perf/PerfHarness.h"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <ctime>
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
#include "project/ProjectFileScanner.h"
#include "project/ProjectTraversalFilter.h"
#include "util/PerformanceCounters.h"
#include "util/TextFileIO.h"
#include "workspace/assist/AssistProviderMerge.h"
#include "workspace/services/SettingsOverlayService.h"
#include "workspace/git/CompareTabReview.h"
#include "workspace/persistence/WorkspacePersistenceFormat.h"
#include "workspace/registries/WorkspaceSettingsRegistry.h"
#include "workspace/shell/WorkspaceShellTestAccess.h"
#include "workspace/state/WorkspaceTabState.h"

namespace microide::tests::perf {
namespace {

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

  std::vector<plugin::registry_interop::StatusItemUpdate> updates;
  updates.reserve(kItems);
  for (int i = 0; i < kItems; ++i) {
    plugin::registry_interop::StatusItemUpdate update;
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
#endif

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

double ThreadCpuMicros() {
  timespec now{};
  ::clock_gettime(CLOCK_THREAD_CPUTIME_ID, &now);
  return static_cast<double>(now.tv_sec) * 1e6 + static_cast<double>(now.tv_nsec) / 1e3;
}

void RunEditorOpenLargeFileAsync(ScenarioContext& context) {
  using TA = workspace::WorkspaceShell::TestAccess;
  using Content = workspace::TabEntry::EditorTabState::Content;
  std::error_code ec;
  const std::filesystem::path dir =
      std::filesystem::temp_directory_path(ec) / "microide_perf_async_open";
  std::filesystem::create_directories(dir, ec);
  const std::filesystem::path file = dir / "large.log";
  constexpr std::uintmax_t kBytes = 64ull << 20;
  if (std::filesystem::file_size(file, ec) != kBytes || ec) {
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    const std::string line =
        "2026-10-08T12:00:00Z INFO request served path=/api/v1/items status=200 bytes=1234\n";
    std::uintmax_t written = 0;
    while (written + line.size() <= kBytes) {
      out << line;
      written += line.size();
    }
    out << std::string(static_cast<std::size_t>(kBytes - written - 1), 'x') << '\n';
  }
  if (!context.Open(dir)) {
    throw std::runtime_error("editor_open_large_file_async: project open failed");
  }
  context.PumpFrames(2);
  workspace::WorkspaceShell& shell = context.Shell();

  context.Measure("open_large_async.open_call", [&] { TA::OpenFile(shell, file); });
  if (TA::ActiveTabContentState(shell) != Content::Loading) {
    throw std::runtime_error("editor_open_large_file_async: a 64 MiB open did not go off-thread");
  }

  constexpr double kStallBudgetUs = 12'000.0;
  double worst_us = 0.0;
  std::size_t frames = 0;
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
  while (TA::ActiveTabContentState(shell) == Content::Loading &&
         std::chrono::steady_clock::now() < deadline) {
    const double t0 = ThreadCpuMicros();
    context.PumpFrames(1);
    worst_us = std::max(worst_us, ThreadCpuMicros() - t0);
    ++frames;
  }
  if (TA::ActiveTabContentState(shell) != Content::Ready) {
    throw std::runtime_error("editor_open_large_file_async: the read never landed");
  }
  if (TA::ActiveEditor(shell).line_count() < 100'000) {
    throw std::runtime_error("editor_open_large_file_async: the tab holds " +
                             std::to_string(TA::ActiveEditor(shell).line_count()) +
                             " lines, not the file");
  }
  if (worst_us > kStallBudgetUs) {
    throw std::runtime_error("editor_open_large_file_async: a frame used " +
                             std::to_string(worst_us) + " us of shell-thread CPU while the read was outstanding (" +
                             std::to_string(frames) + " frames)");
  }

  TA::CloseTab(shell, TA::ActiveTabIndex(shell));
  TA::OpenFile(shell, file);
  if (TA::ActiveTabContentState(shell) != Content::Loading) {
    throw std::runtime_error("editor_open_large_file_async: the reopen did not go off-thread");
  }
  TA::CloseTab(shell, TA::ActiveTabIndex(shell));
  TA::FlushPendingFileReads(shell);
  context.PumpFrames(1);
  if (TA::FocusedGroupOpenTabCount(shell) != 0) {
    throw std::runtime_error("editor_open_large_file_async: a tab closed mid-read came back");
  }
}

void RunMultiCaretRemapBurst(ScenarioContext& context) {
  using editor::detail::ComputeReplacementShape;
  using editor::detail::MultiCaretRemapSite;
  using editor::detail::ResolveMultiCaretRemapSites;

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

void RunSnippetManyMirrorEdit(ScenarioContext& context) {
  constexpr int kMirrors = 150;
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
      for (int k = 0; k < 4; ++k) {
        editor::SnippetTryInsertText(viewport, session, "m");
      }
      volatile std::size_t sink = session.ranges_by_tab[1].size();
      (void)sink;
    }
  });
}

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

void RunBranchReviewPresentationMarkers(ScenarioContext& context) {
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
      ++tab.presentation_revision;
      workspace::ApplyBranchReviewPresentationMarkers(tab, service);
      volatile std::size_t sink = tab.presentation.rows.size();
      (void)sink;
    }
  });

  context.Measure("branch_review.presentation_markers_unchanged", [&]() {
    for (int iter = 0; iter < 12; ++iter) {
      workspace::ApplyBranchReviewPresentationMarkers(tab, service);
      volatile std::size_t sink = tab.presentation.rows.size();
      (void)sink;
    }
  });
}

void RunProjectTraversalFilterScan(ScenarioContext& context) {
  const std::filesystem::path root("/microide-perf/traversal-root");
  project::ProjectTraversalFilter filter(root, {});

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

  const auto scan = [&filter] {
    std::size_t kept = 0;
    for (const auto& [entry, type] : entries) {
      kept += filter.Includes(entry, type) ? 1u : 0u;
    }
    return kept;
  };

  volatile std::size_t warm_sink = scan();
  (void)warm_sink;

  context.Measure("project.traversal_filter_scan", [&]() {
    volatile std::size_t sink = scan();
    (void)sink;
  });
}

void RunProjectTraversalFilterDeepTree(ScenarioContext& context) {
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

  const std::size_t warm_kept = scan();

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
  if (evaluations > 2 * queries) {
    throw std::runtime_error(
        "project_traversal_filter_deep_tree: the rule set ran " + std::to_string(evaluations) +
        " times for " + std::to_string(queries) +
        " queries; it must run a bounded number of times per entry, not once per path component");
  }
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
    .warmup_iterations = 1,
    .tolerance_p95_percent = tolerance::kJitterWallP95,
    .tolerance_max_percent = tolerance::kJitterWallMax,
    .measurement_revision = 2,
    .run = RunProjectTraversalFilterScan,
}});
const ScenarioRegistration g_perf_project_traversal_filter_deep_tree({Scenario{
    .name = "project_traversal_filter_deep_tree",
    .smoke = true,
    .baseline_gated = true,
    .warmup_iterations = 1,
    .tolerance_p95_percent = tolerance::kJitterWallP95,
    .tolerance_max_percent = tolerance::kJitterWallMax,
    .run = RunProjectTraversalFilterDeepTree,
}});

void RunProjectFileScanKernelTree(ScenarioContext& context) {
  const std::filesystem::path fixture = "tests/perf/fixtures/kernel_sized_project";
  if (!RequireFixture(context, fixture, "project_file_scan_kernel_tree")) {
    return;
  }
  std::size_t scanned = 0;
  context.Measure("project.file_scan_kernel_tree", [&]() {
    scanned = project::CollectProjectFiles(fixture, project::ProjectFileScanMode::IncludeHidden)
                  .size();
  });
  if (scanned < 1000) {
    throw std::runtime_error("project_file_scan_kernel_tree: the scan found " +
                             std::to_string(scanned) + " files, the fixture holds 1,200");
  }
}

const ScenarioRegistration g_perf_project_file_scan_kernel_tree({Scenario{
    .name = "project_file_scan_kernel_tree",
    .smoke = false,
    .baseline_gated = true,
    .warmup_iterations = 1,
    .tolerance_p95_percent = tolerance::kJitterWallP95,
    .tolerance_max_percent = tolerance::kJitterWallMax,
    .run = RunProjectFileScanKernelTree,
}});


void RunEditorTabDragBurst(ScenarioContext& context) {
  const std::filesystem::path project = "tests/perf/fixtures/settings_tabs_project";
  if (!RequireFixture(context, project, "editor_tab_drag_burst")) {
    return;
  }
  (void)context.Open(project);
  context.PumpFrames(2);

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

  constexpr int kMotionEvents = 600;
  double damage_pixels = 0.0;
  int full_redraws = 0;

  context.MousePress(grab_x, grab_y);
  context.Measure("tab_drag.motion_burst", [&] {
    for (int i = 0; i < kMotionEvents; ++i) {
      const int period = kMotionEvents / 4;
      const int phase = i % (2 * period);
      const float t = phase < period
                          ? static_cast<float>(phase) / static_cast<float>(period)
                          : 2.0f - static_cast<float>(phase) / static_cast<float>(period);
      const float x = strip.x + t * (strip.w - 1.0f);
      const ScenarioContext::PumpedFrameDamage frame = context.PumpPartialFrame(context.MouseMoveDragging(x, grab_y));
      if (frame.full) {
        ++full_redraws;
      }
      damage_pixels += frame.area;
    }
  });
  context.Measure("tab_drag.drop",
                  [&] { context.MouseRelease(strip.x + strip.w * 0.5f, grab_y); });

  if (full_redraws != 0) {
    throw std::runtime_error("editor_tab_drag_burst: the drag asked for " +
                             std::to_string(full_redraws) +
                             " FULL-WINDOW repaints; a tab drag must damage the strip it is on");
  }
  const double average_areas_per_event =
      damage_pixels / (static_cast<double>(kMotionEvents) * static_cast<double>(strip_area));
  if (average_areas_per_event > 2.0) {
    throw std::runtime_error(
        "editor_tab_drag_burst: the drag damaged " + std::to_string(average_areas_per_event) +
        " tab-strip areas per motion event; the drag repaint must stay scoped to the strip");
  }
  if (damage_pixels <= 0.0) {
    throw std::runtime_error(
        "editor_tab_drag_burst: the drag produced no damage at all, so it measured nothing");
  }
}

const ScenarioRegistration g_perf_editor_open_large_file_async({Scenario{
    .name = "editor_open_large_file_async",
    .smoke = true,
    .baseline_gated = true,
    .warmup_iterations = 1,
    .run = RunEditorOpenLargeFileAsync,
}});

const ScenarioRegistration g_perf_editor_tab_drag_burst({Scenario{
    .name = "editor_tab_drag_burst",
    .smoke = false,
    .baseline_gated = true,
    .warmup_iterations = 1,
    .tolerance_alloc_p50_percent = 3.0,
    .run = RunEditorTabDragBurst,
}});

}
}
