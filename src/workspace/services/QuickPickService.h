#pragma once

#include <cstddef>
#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "plugin/PluginHost.h"
#include "workspace/state/WorkspaceProjectState.h"

namespace microide::workspace {

struct LaunchConfig;

// The quick pick (OverlayMode::QuickPick): one titled, filterable list whose
// rows a kind of command acts on -- VS Code's QuickPick. This owns its life
// cycle: building a kind's items, filtering them as the query changes, and the
// kind's three answers -- what moving the selection previews, what Enter does,
// what Escape puts back. The state it drives lives in the overlay workflow
// (OverlayWorkflowState::quick_pick), where the view model and the key and mouse
// paths already read the other pickers from. Side effects reach the shell
// through Operations, so the service names no shell type.
class QuickPickService {
 public:
  struct Operations {
    // Show the overlay in QuickPick mode; whether it still is; reset its scroll
    // and repaint it.
    std::function<void()> show_overlay;
    std::function<bool()> showing;
    std::function<void()> list_changed;
    // QuickPickKind::LaunchConfig: start debugging with launch config `index`.
    std::function<void(std::size_t)> start_launch_config;
    // QuickPickKind::ColorTheme: apply a scheme, `persist` only when chosen.
    std::function<void(std::string_view, bool persist)> apply_colorscheme;
    // QuickPickKind::DocumentSymbol: put the caret at (line, column) of `path`
    // and bring it into view; and put a view back as it was.
    std::function<void(const std::filesystem::path&, std::size_t, std::size_t)> reveal;
    std::function<void(const QuickPickState::View&)> restore_view;
  };

  void Configure(QuickPickState& state, Operations operations);

  // Show `picker` (built by one of the builders below), filtered by its query.
  void Open(QuickPickState picker);
  // The query changed: filter again from the top match, and preview it.
  void Refilter();
  // The selection moved (a key, a click, the wheel over a row): preview it.
  void SelectionChanged();
  // Enter: the kind's action on the selected match. Accepting ends the preview,
  // so the dismissal that follows restores nothing.
  void Accept();
  // Escape, a click outside, another overlay taking over: undo the preview.
  void Cancel();
  // The items of a pick that opened loading (`kind` still showing), filtered by
  // whatever was typed meanwhile. False when that pick is no longer up.
  bool DeliverItems(QuickPickKind kind, std::vector<QuickPickItem> items);

  // A pick of the project's launch configurations.
  static QuickPickState LaunchConfigs(std::span<const LaunchConfig> configs);
  // A pick of the colour schemes `names`, the active one selected first and
  // restored on Escape.
  static QuickPickState ColorThemes(std::span<const std::string> names, std::string_view active);
  // Go to Symbol in Editor, loading: the symbols arrive through DeliverItems.
  // `view` is where Escape puts the editor back.
  static QuickPickState DocumentSymbolsLoading(QuickPickState::View view);
  // One row per symbol, depth first, nested ones indented under their parent.
  static std::vector<QuickPickItem> DocumentSymbolItems(
      std::span<const plugin::PluginHost::DocumentSymbolNode> symbols);

 private:
  void Preview();

  QuickPickState* state_ = nullptr;
  Operations operations_;
};

}  // namespace microide::workspace
