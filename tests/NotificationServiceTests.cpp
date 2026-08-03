#include "TestSupport.h"

#include "workspace/render/NotificationLayout.h"
#include "workspace/services/NotificationService.h"

#include <limits>
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
  Expect(!service.ExpireDue(near_max),
         "the saturated notification must not be treated as already expired");
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
    const auto toast = NotificationToastLayoutAt(status_bar, 16.0f, 0, 100000.0f);
    Expect(toast.rect.x >= 0.0f, "a toast card must not start left of the window");
    Expect(toast.rect.x + toast.rect.w <= window_width - kNotificationToastMargin + 0.01f,
           "a toast card must stay inside the window margin");
    Expect(toast.text.w > 0.0f && toast.text.x + toast.text.w <= toast.rect.x + toast.rect.w,
           "the toast text box must stay inside its card");
  }
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
  AddTest(tests, "NotificationService/ToneAndEmptyHandling",
          TestNotificationServiceToneAndEmptyHandling);
  AddTest(tests, "NotificationService/ToastWidthScalesWithWindow",
          TestNotificationToastWidthScalesWithWindow);
}

}  // namespace microide::tests
