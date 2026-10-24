#pragma once

#include <cstddef>
#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

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
    // Show the overlay in QuickPick mode; reset its scroll; repaint it.
    std::function<void()> show_overlay;
    std::function<void()> list_changed;
    // QuickPickKind::LaunchConfig: start debugging with launch config `index`.
    std::function<void(std::size_t)> start_launch_config;
    // QuickPickKind::ColorTheme: apply a scheme, `persist` only when chosen.
    std::function<void(std::string_view, bool persist)> apply_colorscheme;
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

  // A pick of the project's launch configurations.
  static QuickPickState LaunchConfigs(std::span<const LaunchConfig> configs);
  // A pick of the colour schemes `names`, the active one selected first and
  // restored on Escape.
  static QuickPickState ColorThemes(std::span<const std::string> names, std::string_view active);

 private:
  void Preview();

  QuickPickState* state_ = nullptr;
  Operations operations_;
};

}  // namespace microide::workspace
