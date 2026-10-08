#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "util/InlineVector.h"
#include "workspace/actions/WorkspaceActionTypes.h"

namespace microide::workspace {

// The sticky row reporting that the file index holds only a prefix of the
// project. Named here rather than spelled at each site because the posting site
// (the redraw pass) and the two clearing sites (the watcher's start and stop) are
// in three different translation units, and a key that only mostly matches is a
// row nothing can dismiss.
inline constexpr std::string_view kProjectIndexTruncatedNotificationKey =
    "project.index.truncated";

// The sticky row reporting that a file is still being read. One key for all of
// them: a second slow open replaces the row rather than stacking, and the row is
// dismissed by the same key wherever a read ends. Named here for the same reason
// as the one above — the posting and dismissing sites are different translation
// units.
inline constexpr std::string_view kFileOpenInProgressNotificationKey = "editor.open.reading";

// One inline button on a notification row. At namespace scope rather than nested
// in NotificationService because the service holds them in an InlineVector, which
// needs the type complete (default member initializers included) at that point.
struct NotificationAction {
  std::string label;  // byte-capped at ingress (NotificationService::MaxActionLabelBytes)
  ActionId id = ActionId::OpenCommandPalette;
  std::vector<std::string> args;
  // A transient row is dismissed once one of its actions runs (the toast has done
  // its job). Set this for an action that should leave the row up — a "Copy" next
  // to a message the user may still be reading.
  bool keep_open = false;
};

// Host-owned notifications ("toasts"). Callers post a short message; the service
// holds no timer of its own (callers pass the current time in SDL_GetTicks ms), so
// it stays deterministic and unit-testable, and the shell schedules a single wake
// at the next expiry instead of polling.
//
// Three properties beyond "a message that fades", each because the version without
// it reported long-running work as either a burst of identical toasts or nothing:
//
//  - **identity** (`key`). A row posted again under the same key REPLACES the one
//    on screen in place. "41 files changed" becoming "58 files changed" is one row
//    that updates, not two rows that stack; and it updates without jumping to the
//    top of the stack, because a row that moves while you are reading it is worse
//    than one that does not change at all.
//  - **lifetime** (`sticky`). Every row used to expire at DurationMs(), including
//    rows that report a STATE — offline, syncing, signed out. Those vanished four
//    seconds after the state they report began and the state was then invisible.
//    A sticky row lives until the state it reports ends and its owner dismisses it
//    by key.
//  - **progress**. A fraction the row renders as a bar. A progress row is sticky by
//    construction: it ends when the work does, and a progress bar that expires
//    mid-progress is a bug rather than a design.
//
// A row may also carry ACTIONS, drawn as VS Code-style inline buttons. An action is
// data — a label plus an ActionId and its arguments — never a closure: a keyed row
// is reposted with a new action set, the control channel lists and invokes them by
// label, and running one goes through the same action executor as a menu item.
class NotificationService {
 public:
  enum class Tone { Info, Warning, Error };

  using Action = NotificationAction;
  static constexpr std::size_t MaxActions() { return 3; }
  using Actions = util::InlineVector<Action, 3>;

  struct Notification {
    Tone tone = Tone::Info;
    std::string key;      // empty for an anonymous row; see Show()
    std::string message;  // already byte-capped at ingress (see MaxMessageBytes)
    std::uint64_t expiry_ms = 0;  // ignored while sticky
    bool truncated = false;  // true when the original message exceeded MaxMessageBytes
    // Lives until DismissKey rather than until expiry_ms. A FIELD rather than a
    // sentinel expiry: UINT64_MAX is already what a transient expiry saturates to
    // near the clock's end, so overloading it made a four-second toast posted at
    // the wrong moment permanent.
    bool sticky = false;
    // 0..1 when this row reports progress. Present implies sticky.
    std::optional<float> progress;
    Actions actions;
    // The pointer is over this row: it does not expire while it is (see SetHovered).
    bool hovered = false;
    // Index into `actions` of the button under the pointer.
    std::optional<std::size_t> hovered_action;
    // Keyboard focus (see Focus): the row does not expire while focused, and
    // `focused_action` is the button Enter runs.
    bool focused = false;
    std::optional<std::size_t> focused_action;
  };

  // Keys the focused stack understands, already translated from the platform's
  // key events so the service stays free of any windowing library.
  enum class FocusKey {
    Older,     // Up: the row above
    Newer,     // Down: the row below
    Previous,  // Left / Shift+Tab: the previous button, wrapping
    Next,      // Right / Tab: the next button, wrapping
    Activate,  // Enter / Space: run the focused button
    Close,     // Delete: dismiss the focused row, focus moves to its neighbour
    Leave,     // Escape: give focus back, the toasts stay
  };
  struct FocusKeyResult {
    bool changed = false;          // something a frame draws changed
    std::optional<Action> action;  // Activate on a button: run this
  };

  // What a caller posts. The two-argument Show below covers the common case; this
  // is for a row that needs identity, a lifetime or progress.
  struct Request {
    Tone tone = Tone::Info;
    // Stable across updates of the same logical row. Empty means anonymous, which
    // keeps the old behaviour: dedup by exact tone-and-text match.
    std::string key;
    std::string message;
    // Lives until DismissKey. Requires a key — without one nothing could ever
    // dismiss it, so a keyless sticky request is treated as transient.
    bool sticky = false;
    std::optional<float> progress;  // implies sticky
    // At most MaxActions(); extras are dropped. Reposting a keyed row replaces its
    // actions in place along with its text.
    std::vector<Action> actions;
  };

  // Map a plugin-supplied level string to a tone ("warning"/"warn" -> Warning,
  // "error"/"err" -> Error, anything else -> Info).
  static Tone ToneFromLevel(std::string_view level);

  // Post a transient row expiring at now_ms + DurationMs(). Empty messages are
  // ignored. A message identical to one still on screen (same tone and text)
  // refreshes that one's expiry instead of stacking a duplicate.
  void Show(Tone tone, std::string message, std::uint64_t now_ms);

  // Post or update a row. A request carrying a key replaces the row with that key
  // IN PLACE — same stack position, new text/tone/progress, refreshed expiry —
  // rather than removing it and appending, so an updating row does not walk up the
  // stack under the pointer.
  void Show(Request request, std::uint64_t now_ms);

  // Remove the row with this key. Returns true if one was there (the caller should
  // then request a redraw). This is how a sticky row ends.
  bool DismissKey(std::string_view key);

  // Remove notifications whose expiry has passed. Returns true if any were removed
  // (the caller should then request a redraw). Sticky rows never expire.
  bool ExpireDue(std::uint64_t now_ms);

  // Milliseconds until the earliest expiry (0 if already due), or nullopt when no
  // row expires on its own.
  std::optional<std::uint64_t> NextExpiryDelayMs(std::uint64_t now_ms) const;

  // Mark the row at `index` (nullopt: none) as under the pointer, and which of its
  // buttons is. A hovered row does not expire; when the pointer leaves a row that
  // is already past its expiry, the row gets HoverGraceMs() more rather than
  // vanishing the instant the pointer moves off it. Returns true when anything a
  // frame draws changed.
  bool SetHovered(std::optional<std::size_t> index, std::optional<std::size_t> action,
                  std::uint64_t now_ms);

  // The action at (`index`, `action`), copied out because running it may post or
  // dismiss rows. Dismisses a transient row unless the action keeps it open.
  // nullopt for a stale index.
  std::optional<Action> TakeAction(std::size_t index, std::size_t action);

  // Resolve a row by key, else by its index in Active() (oldest first) — the two
  // ways an agent names a row through the control channel.
  std::optional<std::size_t> FindRow(std::string_view key_or_index) const;
  // Resolve one of that row's buttons by label (exact, then ASCII case-insensitive),
  // else by index.
  std::optional<std::size_t> FindAction(std::size_t row, std::string_view label_or_index) const;

  // Keyboard focus on the stack (VS Code's "Focus Notification Toast"): the newest
  // row, on its primary (last) button. False when there is nothing to focus.
  bool Focus();
  bool HasFocus() const;
  void ClearFocus();
  FocusKeyResult HandleFocusKey(FocusKey key);

  // Drop a single notification by its index in Active(). Out-of-range indices are
  // ignored, so a click resolved against a stale frame cannot corrupt the stack.
  void Dismiss(std::size_t index) {
    if (index < notifications_.size()) {
      notifications_.erase(notifications_.begin() + static_cast<std::ptrdiff_t>(index));
    }
  }

  const std::vector<Notification>& Active() const { return notifications_; }
  bool Empty() const { return notifications_.empty(); }
  void Clear() { notifications_.clear(); }

  static constexpr std::uint64_t DurationMs() { return 4000; }
  // A row with buttons asks the user to do something; four seconds is not long
  // enough to read it and reach for the mouse.
  static constexpr std::uint64_t ActionDurationMs() { return 10000; }
  static constexpr std::uint64_t HoverGraceMs() { return 1500; }
  static constexpr std::size_t MaxActionLabelBytes() { return 40; }
  // Transient rows on screen at once. Sticky rows do not count against this — they
  // report a state, and dropping one because three warnings arrived would hide the
  // state rather than the warnings.
  static constexpr std::size_t MaxVisible() { return 4; }
  // But the stack still cannot grow without bound: the toasts are laid out upward
  // from the status bar, so enough of them walk off the top of the window. Sticky
  // rows past this are refused rather than silently stacked off screen.
  static constexpr std::size_t MaxSticky() { return 3; }
  // Ingress byte cap for a single toast. A toast is clipped to ~320px on screen, so no
  // visible message needs more than a few hundred bytes; capping here stops a plugin
  // `ctx.notify` or a subprocess/provider error string from forcing large string copies
  // and text measurement during a full redraw (TD-2026-07-17A-101).
  static constexpr std::size_t MaxMessageBytes() { return 512; }

 private:
  // Shared ingress: byte-cap on a codepoint boundary and append the clipped marker.
  static bool CapMessage(std::string& message);
  void TrimTransientOverflow();

  std::vector<Notification> notifications_;
};

}  // namespace microide::workspace
