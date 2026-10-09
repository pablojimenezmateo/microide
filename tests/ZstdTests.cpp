#include "TestSupport.h"

#include "util/Zstd.h"

#include <string>
#include <vector>

namespace microide::tests {
namespace {

// A source file of `lines` distinct lines, like the ones agents rewrite.
std::string SourceFile(std::size_t lines) {
  std::string text;
  for (std::size_t i = 0; i < lines; ++i) {
    text += "int function_" + std::to_string(i) + "(int x) { return x * " + std::to_string(i * 7) +
            " + " + std::to_string(i % 13) + "; }\n";
  }
  return text;
}

// The case deltas exist for: one function rewritten in a ~200 KiB file costs a
// handful of bytes on the wire, and the other side gets the exact bytes back.
void TestZstdDeltaOfAOneFunctionEditIsSmall() {
  const std::string base = SourceFile(4000);
  std::string target = base;
  const std::size_t at = target.find("int function_2000(");
  target.replace(at, target.find('\n', at) - at, "int function_2000(int x) { return -x; }");
  const auto delta = util::ZstdDelta(base, target);
  Expect(delta.has_value(), "a delta is made");
  if (!delta) {
    return;
  }
  Expect(delta->size() < 256, "a one-line edit to " + std::to_string(base.size()) +
                                  " bytes is a tiny delta: " + std::to_string(delta->size()));
  const auto back = util::ZstdApplyDelta(base, *delta, target.size());
  Expect(back.has_value() && *back == target, "the target comes back byte for byte");
}

// What a delta must never do is be applied as if it were something else.
void TestZstdApplyDeltaRefusesWhatItCannotTrust() {
  const std::string base = SourceFile(200);
  const std::string target = base + "int added(void);\n";
  const auto delta = util::ZstdDelta(base, target);
  Expect(delta.has_value(), "a delta is made");
  if (!delta) {
    return;
  }
  Expect(!util::ZstdApplyDelta(base, *delta, target.size() - 1).has_value(),
         "a result over the caller's limit is refused before it is allocated");
  for (std::size_t cut : {std::size_t{0}, std::size_t{1}, delta->size() / 2, delta->size() - 1}) {
    Expect(!util::ZstdApplyDelta(base, delta->substr(0, cut), target.size()).has_value(),
           "a truncated frame is refused at " + std::to_string(cut));
  }
  Expect(!util::ZstdApplyDelta(base, "not a zstd frame", 1 << 20).has_value(), "garbage is refused");
  const auto empty = util::ZstdDelta({}, target);
  Expect(empty.has_value() && util::ZstdApplyDelta({}, *empty, target.size()) == target,
         "an empty base is plain compression");
}

}  // namespace

void RegisterZstdTests(std::vector<TestCase>& tests) {
  AddTest(tests, "Zstd/DeltaOfAOneFunctionEditIsSmall", TestZstdDeltaOfAOneFunctionEditIsSmall);
  AddTest(tests, "Zstd/ApplyDeltaRefusesWhatItCannotTrust", TestZstdApplyDeltaRefusesWhatItCannotTrust);
}

}  // namespace microide::tests
