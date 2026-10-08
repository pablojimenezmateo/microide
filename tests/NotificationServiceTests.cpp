#include "TestSupport.h"

#include "workspace/render/NotificationLayout.h"
#include "workspace/services/NotificationService.h"

#include <limits>
#include <string>
#include <vector>

namespace microide::tests {
namespace {

using microide::workspace::NotificationService;

void TestNotificationServiceExpiresAfterDuration() {
  NotificationService service;
  service.Show(NotificationService::Tone::Info, "hello", 1000);
  Expect(service.Active().size() == 1, "showing a notification should make it active");
  Expect(service.Active().front().expiry_ms == 1000 + NotificationService::DurationMs(),
         "expiry should be now + duration");

  // Before expiry: still present, and the wake delay counts down.
  Expect(!service.ExpireDue(1000 + NotificationService::DurationMs() - 1),
         "a not-yet-expired notification must not be removed");
  Expect(service.NextExpiryDelayMs(1000).value_or(0) == NotificationService::DurationMs(),
         "next-expiry delay should equal the remaining lifetime");

  // At/after expiry: removed, and ExpireDue reports the change (so caller redraws).
  Expect(service.ExpireDue(1000 + NotificationService::DurationMs()),
         "an expired notification should be removed and report a change");
  Expect(service.Empty(), "no notifications should remain after expiry");
  Expect(!service.NextExpiryDelayMs(0).has_value(),
         "an empty service should report no scheduled wake");
}

void TestNotificationServiceDelayClampsToZeroWhenDue() {
  NotificationService service;
  service.Show(NotificationService::Tone::Warning, "warn", 0);
  Expect(service.NextExpiryDelayMs(NotificationService::DurationMs() + 5) == 0,
         "an overdue notification should report a zero (immediate) wake delay");
}

void TestNotificationServiceDropsOldestBeyondMax() {
  NotificationService service;
  for (std::size_t i = 0; i < NotificationService::MaxVisible() + 2; ++i) {
    service.Show(NotificationService::Tone::Info, "n" + std::to_string(i), 0);
  }
  Expect(service.Active().size() == NotificationService::MaxVisible(),
         "the queue should cap at MaxVisible entries");
  Expect(service.Active().front().message == "n2",
         "the two oldest notifications should have been dropped");
  Expect(service.Active().back().message ==
             "n" + std::to_string(NotificationService::MaxVisible() + 1),
         "the newest notification should be retained at the back");
}

void TestNotificationServiceExpirySaturatesNearMax() {
  NotificationService service;
  // A monotonic clock near UINT64_MAX must not wrap the expiry to a tiny value
  // (which would drop the notification immediately). It saturates instead.
  const std::uint64_t near_max = std::numeric_limits<std::uint64_t>::max() - 1;
  service.Show(NotificationService::Tone::Info, "late", near_max);
  Expect(service.Active().size() == 1, "notification shown near clock max stays active");
  Expect(service.Active().front().expiry_ms == std::numeric_limits<std::uint64_t>::max(),
         "expiry should saturate at UINT64_MAX rather than wrap");
  // Stickiness is a FIELD, not this saturated value: an earlier draft made
  // UINT64_MAX mean "never expires", which would have turned exactly this
  // four-second toast into a permanent one.
  Expect(!service.Active().front().sticky,
         "a row posted without a key or progress is never sticky");
  Expect(!service.ExpireDue(near_max),
         "the saturated notification must not be treated as already expired");
}

// Repeating a message refreshes the toast already on screen instead of stacking a
// duplicate: a refused action fired from a held shortcut, or a button clicked twice,
// would otherwise push every other toast out of the four-deep stack with copies of
// one sentence.
void TestNotificationServiceCollapsesRepeatedMessage() {
  NotificationService service;
  service.Show(NotificationService::Tone::Warning, "no repo here", 1000);
  service.Show(NotificationService::Tone::Warning, "no repo here", 3000);
  Expect(service.Active().size() == 1, "an identical repeat should not stack a second toast");
  Expect(service.Active().front().expiry_ms == 3000 + NotificationService::DurationMs(),
         "the repeat should refresh the existing toast's expiry");

  // Same text at a different tone is a different message, and stacks.
  service.Show(NotificationService::Tone::Error, "no repo here", 3000);
  Expect(service.Active().size() == 2, "a different tone is a distinct notification");
  service.Show(NotificationService::Tone::Warning, "something else", 3000);
  Expect(service.Active().size() == 3, "different text still stacks");
}

void TestNotificationServiceToneAndEmptyHandling() {
  Expect(NotificationService::ToneFromLevel("error") == NotificationService::Tone::Error,
         "'error' should map to the error tone");
  Expect(NotificationService::ToneFromLevel("warn") == NotificationService::Tone::Warning,
         "'warn' should map to the warning tone");
  Expect(NotificationService::ToneFromLevel("anything") == NotificationService::Tone::Info,
         "unknown levels should fall back to info");

  NotificationService service;
  service.Show(NotificationService::Tone::Info, "", 0);
  Expect(service.Empty(), "empty messages should be ignored");
}

// A single oversized toast is byte-capped at ingress on a UTF-8 boundary and flagged
// truncated, so a plugin ctx.notify or a subprocess error string can't force large string
// copies + text measurement during a full redraw (TD-2026-07-17A-101).
void TestNotificationServiceByteCapsOversizedMessage() {
  NotificationService service;

  // A short message is stored verbatim, not flagged.
  service.Show(NotificationService::Tone::Info, "short", 0);
  Expect(service.Active().back().message == "short" && !service.Active().back().truncated,
         "a short message is stored unchanged and not marked truncated");

  // An oversized message is truncated to <= the byte cap (+ the multi-byte ellipsis
  // marker) and flagged.
  const std::string huge(NotificationService::MaxMessageBytes() * 4, 'x');
  service.Show(NotificationService::Tone::Error, huge, 0);
  const auto& capped = service.Active().back();
  Expect(capped.truncated, "an oversized message is flagged truncated");
  Expect(capped.message.size() <= NotificationService::MaxMessageBytes() + 4,
         "the stored message is bounded by the byte cap plus the ellipsis marker");
  Expect(capped.message.size() < huge.size(), "the stored message is shorter than the input");

  // Truncation must not split a multi-byte codepoint: a message of many 2-byte
  // codepoints ("é") truncated mid-run keeps whole codepoints (the byte just past the
  // kept prefix, before the ellipsis, is not a UTF-8 continuation byte).
  std::string accents;
  while (accents.size() <= NotificationService::MaxMessageBytes() * 2) {
    accents += "\xC3\xA9";  // U+00E9 é
  }
  service.Show(NotificationService::Tone::Info, accents, 0);
  const std::string& acc_msg = service.Active().back().message;
  // Strip the trailing "…" (3 bytes) then verify the kept prefix is an even number of
  // bytes (whole 2-byte codepoints) with no dangling lead byte.
  Expect(service.Active().back().truncated, "the accented message is truncated");
  const std::string ellipsis = "…";
  Expect(acc_msg.size() >= ellipsis.size() &&
             acc_msg.compare(acc_msg.size() - ellipsis.size(), ellipsis.size(), ellipsis) == 0,
         "a truncated message ends with the ellipsis marker");
  const std::size_t kept = acc_msg.size() - ellipsis.size();
  Expect(kept % 2 == 0, "truncation kept whole 2-byte codepoints (no split multi-byte sequence)");
}

// Toast cards were capped at a flat 320px of text regardless of window size, so
// any message past ~40 characters was sheared off mid-word against the card edge
// on a 1440px window with hundreds of pixels to spare. The budget now scales with
// the window between a readable floor and a "do not span the screen" ceiling.
void TestNotificationToastWidthScalesWithWindow() {
  using microide::workspace::NotificationToastLayoutAt;
  using microide::workspace::NotificationToastTextBudget;
  using microide::workspace::kNotificationToastMargin;
  using microide::workspace::kNotificationToastMaxTextWidth;
  using microide::workspace::kNotificationToastMinTextWidth;

  Expect(NotificationToastTextBudget(1440.0f) > 320.0f,
         "a wide window should give a toast more room than the old flat cap");
  Expect(NotificationToastTextBudget(320.0f) == kNotificationToastMinTextWidth,
         "a narrow window should still get the readable floor");
  Expect(NotificationToastTextBudget(4000.0f) == kNotificationToastMaxTextWidth,
         "an ultrawide window must not let one message span the screen");

  // Whatever the budget, the card stays inside the window with its margin.
  for (const float window_width : {320.0f, 1440.0f, 4000.0f}) {
    const SDL_FRect status_bar{0.0f, 700.0f, window_width, 20.0f};
    const auto toast = NotificationToastLayoutAt(
        status_bar, 16.0f, microide::workspace::NotificationStackBottom(status_bar), 100000.0f);
    Expect(toast.rect.x >= 0.0f, "a toast card must not start left of the window");
    Expect(toast.rect.x + toast.rect.w <= window_width - kNotificationToastMargin + 0.01f,
           "a toast card must stay inside the window margin");
    Expect(toast.text.w > 0.0f && toast.text.x + toast.text.w <= toast.rect.x + toast.rect.w,
           "the toast text box must stay inside its card");

    // The progress track lives INSIDE the card, along its bottom edge. That is the
    // load-bearing part: the geometry a click resolves against must be the same
    // rect whether or not the row reports progress, or a progress row's card
    // would be painted somewhere the hit-test does not look.
    Expect(toast.progress_track.x >= toast.rect.x &&
               toast.progress_track.x + toast.progress_track.w <=
                   toast.rect.x + toast.rect.w + 0.01f,
           "the progress track must stay within the card horizontally");
    Expect(toast.progress_track.y >= toast.rect.y &&
               toast.progress_track.y + toast.progress_track.h <=
                   toast.rect.y + toast.rect.h + 0.01f,
           "the progress track must sit inside the card, not below it");
    Expect(toast.progress_track.h > 0.0f, "the progress track must be drawable");
  }

  // Two rows of the same measured width occupy the same card whether or not one
  // of them carries progress: the layout takes no progress argument, which is how
  // that stays true by construction rather than by two call sites agreeing.
  const SDL_FRect status_bar{0.0f, 700.0f, 1280.0f, 20.0f};
  const auto plain = NotificationToastLayoutAt(
      status_bar, 16.0f, microide::workspace::NotificationStackBottom(status_bar), 300.0f);
  const auto second = NotificationToastLayoutAt(
      status_bar, 16.0f, plain.rect.y - microide::workspace::kNotificationToastGap, 300.0f);
  Expect(second.rect.y < plain.rect.y, "older toasts stack upward from the status bar");
  Expect(second.rect.h == plain.rect.h, "a progress row does not get a taller card");
}


// A keyed row REPLACES the one on screen in place. Both halves matter: "41 files
// changed" becoming "58 files changed" must be one row, and it must not jump to
// the top of the stack while the pointer is over it.
void TestNotificationServiceKeyedRowUpdatesInPlace() {
  NotificationService service;
  service.Show(NotificationService::Tone::Info, "first", 0);
  service.Show(NotificationService::Request{.key = "sync", .message = "41 files changed"}, 0);
  service.Show(NotificationService::Tone::Info, "last", 0);
  Expect(service.Active().size() == 3, "three distinct rows");

  service.Show(NotificationService::Request{.key = "sync", .message = "58 files changed"}, 10);
  Expect(service.Active().size() == 3, "an update replaces rather than stacks");
  Expect(service.Active()[1].message == "58 files changed",
         "the keyed row updates IN PLACE — same stack position, new text");
  Expect(service.Active()[2].message == "last",
         "and the rows around it do not move");

  // A different key is a different row.
  service.Show(NotificationService::Request{.key = "build", .message = "building"}, 10);
  Expect(service.Active().size() == 4, "a different key is a different row");
}

// A row that reports a STATE must outlive DurationMs. Before this every row
// expired at four seconds, so "offline" disappeared four seconds after going
// offline and the state was then invisible.
void TestNotificationServiceStickyRowOutlivesTheTimer() {
  NotificationService service;
  service.Show(NotificationService::Request{
                   .key = "conn", .message = "Disconnected from build-box", .sticky = true},
               0);
  Expect(service.Active().size() == 1 && service.Active().front().sticky,
         "a keyed sticky row is posted sticky");
  Expect(!service.ExpireDue(NotificationService::DurationMs() * 100),
         "a sticky row does not expire on the timer");
  Expect(service.Active().size() == 1, "and is still on screen");
  Expect(!service.NextExpiryDelayMs(0).has_value(),
         "a stack of only sticky rows schedules no wake — there is nothing to expire");

  Expect(service.DismissKey("conn"), "its owner ends it by key");
  Expect(service.Empty(), "and it is gone");
  Expect(!service.DismissKey("conn"), "dismissing it twice is not an error");
}

// A sticky row with no key could never be dismissed by anyone. Posting it
// transient is the lesser failure: a permanent toast nothing can remove would sit
// over the status bar for the rest of the session.
void TestNotificationServiceKeylessStickyIsRefused() {
  NotificationService service;
  service.Show(NotificationService::Request{.message = "no key", .sticky = true}, 0);
  Expect(service.Active().size() == 1, "the message is still shown");
  Expect(!service.Active().front().sticky, "but not as a row nothing can dismiss");
  Expect(service.ExpireDue(NotificationService::DurationMs() + 1), "it expires normally");
}

// Progress implies sticky (a bar that vanishes mid-progress is a bug), and sticky
// rows do not count against the transient cap — dropping the "syncing" row because
// four warnings arrived would hide the state rather than the warnings.
void TestNotificationServiceProgressIsStickyAndOutsideTheVisibleCap() {
  NotificationService service;
  service.Show(NotificationService::Request{
                   .key = "sync", .message = "Syncing", .progress = 0.25f},
               0);
  Expect(service.Active().front().sticky, "a progress row is sticky by construction");
  Expect(service.Active().front().progress.has_value() &&
             service.Active().front().progress.value() == 0.25f,
         "and carries its fraction");

  for (std::size_t i = 0; i < NotificationService::MaxVisible() + 2; ++i) {
    service.Show(NotificationService::Tone::Warning, "warning " + std::to_string(i), 0);
  }
  std::size_t transient = 0;
  bool kept_progress = false;
  for (const auto& row : service.Active()) {
    transient += row.sticky ? 0 : 1;
    kept_progress = kept_progress || row.key == "sync";
  }
  Expect(transient == NotificationService::MaxVisible(),
         "transient rows are still capped at MaxVisible");
  Expect(kept_progress, "the sticky progress row survives a burst of warnings");

  service.Show(NotificationService::Request{
                   .key = "sync", .message = "Syncing", .progress = 0.9f},
               0);
  Expect(service.Active().front().progress.value() == 0.9f,
         "and updates its fraction in place");
}

// Sticky rows are not unbounded either: the stack lays out upward from the status
// bar, so enough of them walk off the top of the window where nobody can dismiss
// them.
void TestNotificationServiceStickyStackIsBounded() {
  NotificationService service;
  for (std::size_t i = 0; i < NotificationService::MaxSticky() + 3; ++i) {
    service.Show(NotificationService::Request{.key = "k" + std::to_string(i),
                                              .message = "state " + std::to_string(i),
                                              .sticky = true},
                 0);
  }
  Expect(service.Active().size() == NotificationService::MaxSticky(),
         "sticky rows past the cap are refused rather than stacked off screen");
  Expect(service.Active().front().key == "k0", "and the ones already there are kept");
}

// Actions are data on the row: capped at MaxActions, labels byte-capped, an empty
// label dropped, and a row with buttons lives longer than a plain one.
void TestNotificationServiceActionsAreCappedData() {
  using microide::workspace::ActionId;
  NotificationService service;
  NotificationService::Request request{.message = "formatter failed"};
  request.actions.push_back({.label = "", .id = ActionId::Goto});
  for (int i = 0; i < 5; ++i) {
    request.actions.push_back({.label = "Action " + std::to_string(i), .id = ActionId::Goto,
                               .args = {std::to_string(i)}});
  }
  request.actions[1].label = std::string(200, 'x');
  service.Show(std::move(request), 0);
  const auto& row = service.Active().at(0);
  Expect(row.actions.size() == NotificationService::MaxActions(),
         "a row carries at most MaxActions buttons");
  Expect(row.actions[0].label.size() <= NotificationService::MaxActionLabelBytes() + 3,
         "a long label is byte-capped at ingress");
  Expect(row.actions[1].label == "Action 1" && row.actions[1].args.at(0) == "1",
         "an empty label is dropped and the next action keeps its arguments");
  Expect(row.expiry_ms == NotificationService::ActionDurationMs(),
         "a row with buttons gets the longer lifetime");
  service.Show(NotificationService::Tone::Info, "plain", 0);
  Expect(service.Active().at(1).expiry_ms == NotificationService::DurationMs(),
         "a plain row keeps the short lifetime");
}

// Reposting a keyed row replaces its buttons in place, without moving the row.
void TestNotificationServiceKeyedRowReplacesActionsInPlace() {
  using microide::workspace::ActionId;
  NotificationService service;
  NotificationService::Request first{.key = "remote", .message = "Disconnected", .sticky = true};
  first.actions.push_back({.label = "Reconnect", .id = ActionId::Goto});
  first.actions.push_back({.label = "Show Log", .id = ActionId::Goto});
  service.Show(std::move(first), 0);
  service.Show(NotificationService::Tone::Info, "newer", 0);
  Expect(service.SetHovered(0, 1, 0), "hovering the second button is a visible change");

  NotificationService::Request second{.key = "remote", .message = "Reconnecting", .sticky = true};
  second.actions.push_back({.label = "Cancel", .id = ActionId::Goto});
  service.Show(std::move(second), 5);
  Expect(service.Active().size() == 2, "a repost does not stack");
  Expect(service.Active()[0].message == "Reconnecting" && service.Active()[0].actions.size() == 1 &&
             service.Active()[0].actions[0].label == "Cancel",
         "the keyed row's text and buttons are replaced in place");
  Expect(!service.Active()[0].hovered_action.has_value(),
         "a hovered button that no longer exists stops being hovered");
}

// Running an action dismisses a transient row unless the action keeps it open; a
// sticky row reports a state and stays until its owner dismisses it.
void TestNotificationServiceTakeActionDismissRules() {
  using microide::workspace::ActionId;
  NotificationService service;
  NotificationService::Request transient{.message = "saved unformatted"};
  transient.actions.push_back({.label = "Copy", .id = ActionId::Goto, .keep_open = true});
  transient.actions.push_back({.label = "Show Output", .id = ActionId::Goto, .args = {"7"}});
  service.Show(std::move(transient), 0);

  auto kept = service.TakeAction(0, 0);
  Expect(kept.has_value() && kept->label == "Copy", "the action is returned");
  Expect(service.Active().size() == 1, "a keep_open action leaves the row up");
  auto taken = service.TakeAction(0, 1);
  Expect(taken.has_value() && taken->args.at(0) == "7", "the action carries its arguments");
  Expect(service.Empty(), "a transient row is dismissed after its action runs");
  Expect(!service.TakeAction(0, 0).has_value(), "a stale index resolves to nothing");

  NotificationService::Request sticky{.key = "state", .message = "Offline", .sticky = true};
  sticky.actions.push_back({.label = "Reconnect", .id = ActionId::Goto});
  service.Show(std::move(sticky), 0);
  Expect(service.TakeAction(0, 0).has_value() && service.Active().size() == 1,
         "a sticky row survives its action");
}

// A hovered toast does not expire under the pointer, and leaving it grants a grace
// period instead of removing it on the motion event that left.
void TestNotificationServiceHoveredRowDoesNotExpire() {
  NotificationService service;
  service.Show(NotificationService::Tone::Info, "hold me", 0);
  Expect(service.SetHovered(0, std::nullopt, 100), "entering a row is a change");
  Expect(!service.SetHovered(0, std::nullopt, 200), "staying on it is not");
  Expect(!service.ExpireDue(NotificationService::DurationMs() * 10),
         "a hovered row does not expire");
  Expect(!service.NextExpiryDelayMs(0).has_value(), "nothing to wake for while hovered");

  const std::uint64_t left_at = NotificationService::DurationMs() * 10;
  Expect(service.SetHovered(std::nullopt, std::nullopt, left_at), "leaving is a change");
  Expect(!service.ExpireDue(left_at), "leaving an expired row does not remove it at once");
  Expect(service.ExpireDue(left_at + NotificationService::HoverGraceMs()),
         "it expires once the grace period passes");
}

// Keyboard focus (Focus Notifications): lands on the newest row's primary button,
// arrows walk rows and buttons, Enter hands back the focused action, Delete
// dismisses and moves to a neighbour, Escape leaves. A focused row does not expire.
void TestNotificationServiceKeyboardFocus() {
  using microide::workspace::ActionId;
  using FocusKey = NotificationService::FocusKey;
  NotificationService service;
  Expect(!service.Focus(), "an empty stack cannot take focus");
  service.Show(NotificationService::Request{.key = "older", .message = "older", .sticky = true}, 0);
  NotificationService::Request request{.message = "newer"};
  request.actions.push_back({.label = "A", .id = ActionId::Goto, .args = {"1"}});
  request.actions.push_back({.label = "B", .id = ActionId::Goto, .args = {"2"}});
  service.Show(std::move(request), 0);

  Expect(service.Focus() && service.HasFocus(), "focus lands on the stack");
  Expect(service.Active()[1].focused && service.Active()[1].focused_action == std::size_t{1},
         "on the newest row's primary (last) button");
  Expect(!service.ExpireDue(1'000'000) && service.Active().size() == 2,
         "a focused row does not expire");

  Expect(service.HandleFocusKey(FocusKey::Next).changed &&
             service.Active()[1].focused_action == std::size_t{0},
         "Next wraps to the first button");
  Expect(service.HandleFocusKey(FocusKey::Previous).changed &&
             service.Active()[1].focused_action == std::size_t{1},
         "Previous wraps back");
  Expect(!service.HandleFocusKey(FocusKey::Newer).changed, "nothing below the newest row");
  Expect(service.HandleFocusKey(FocusKey::Older).changed && service.Active()[0].focused &&
             !service.Active()[1].focused,
         "Older moves up the stack");
  Expect(!service.HandleFocusKey(FocusKey::Activate).action.has_value(),
         "a row without buttons has nothing to activate");
  Expect(service.HandleFocusKey(FocusKey::Newer).changed, "back down");

  const auto activated = service.HandleFocusKey(FocusKey::Activate);
  Expect(activated.action.has_value() && activated.action->args.at(0) == "2",
         "Enter returns the focused button's action");
  Expect(service.Active().size() == 1 && !service.HasFocus(),
         "the transient row closed behind its action and took the focus with it");

  Expect(service.Focus(), "refocus");
  Expect(service.HandleFocusKey(FocusKey::Close).changed && service.Empty() && !service.HasFocus(),
         "Delete dismisses the focused row");
  service.Show(NotificationService::Tone::Info, "x", 0);
  Expect(service.Focus() && service.HandleFocusKey(FocusKey::Leave).changed && !service.HasFocus() &&
             service.Active().size() == 1,
         "Escape gives focus back and keeps the row");
}

}  // namespace

void RegisterNotificationServiceTests(std::vector<TestCase>& tests) {
  AddTest(tests, "NotificationService/ByteCapsOversizedMessage",
          TestNotificationServiceByteCapsOversizedMessage);
  AddTest(tests, "NotificationService/ExpiresAfterDuration",
          TestNotificationServiceExpiresAfterDuration);
  AddTest(tests, "NotificationService/DelayClampsToZeroWhenDue",
          TestNotificationServiceDelayClampsToZeroWhenDue);
  AddTest(tests, "NotificationService/DropsOldestBeyondMax",
          TestNotificationServiceDropsOldestBeyondMax);
  AddTest(tests, "NotificationService/ExpirySaturatesNearMax",
          TestNotificationServiceExpirySaturatesNearMax);
  AddTest(tests, "NotificationService/CollapsesRepeatedMessage",
          TestNotificationServiceCollapsesRepeatedMessage);
  AddTest(tests, "NotificationService/ToneAndEmptyHandling",
          TestNotificationServiceToneAndEmptyHandling);
  AddTest(tests, "NotificationService/KeyedRowUpdatesInPlace",
          TestNotificationServiceKeyedRowUpdatesInPlace);
  AddTest(tests, "NotificationService/StickyRowOutlivesTheTimer",
          TestNotificationServiceStickyRowOutlivesTheTimer);
  AddTest(tests, "NotificationService/KeylessStickyIsRefused",
          TestNotificationServiceKeylessStickyIsRefused);
  AddTest(tests, "NotificationService/ProgressIsStickyAndOutsideTheVisibleCap",
          TestNotificationServiceProgressIsStickyAndOutsideTheVisibleCap);
  AddTest(tests, "NotificationService/StickyStackIsBounded",
          TestNotificationServiceStickyStackIsBounded);
  AddTest(tests, "NotificationService/ActionsAreCappedData",
          TestNotificationServiceActionsAreCappedData);
  AddTest(tests, "NotificationService/KeyedRowReplacesActionsInPlace",
          TestNotificationServiceKeyedRowReplacesActionsInPlace);
  AddTest(tests, "NotificationService/TakeActionDismissRules",
          TestNotificationServiceTakeActionDismissRules);
  AddTest(tests, "NotificationService/HoveredRowDoesNotExpire",
          TestNotificationServiceHoveredRowDoesNotExpire);
  AddTest(tests, "NotificationService/KeyboardFocus", TestNotificationServiceKeyboardFocus);
  AddTest(tests, "NotificationService/ToastWidthScalesWithWindow",
          TestNotificationToastWidthScalesWithWindow);
}

}  // namespace microide::tests
