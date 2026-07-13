#include "TestSupport.h"

#include "workspace/NotificationService.h"

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

}  // namespace

void RegisterNotificationServiceTests(std::vector<TestCase>& tests) {
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
}

}  // namespace microide::tests
