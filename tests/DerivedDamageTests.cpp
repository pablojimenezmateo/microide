#include "TestSupport.h"

#include "workspace/render/DerivedDamage.h"
#include "workspace/render/PreparedFrameState.h"

#include <vector>

namespace microide::tests {
namespace {

using workspace::DerivedDamage;
using workspace::DerivedSurface;
using workspace::PaintFingerprint;

std::vector<SDL_FRect> Commit(DerivedDamage& damage, const std::vector<DerivedDamage::Unit>& units) {
  std::vector<DerivedDamage::Unit>& frame = damage.Begin(DerivedSurface::StatusBar);
  frame = units;
  std::vector<SDL_FRect> damaged;
  damage.Commit(DerivedSurface::StatusBar, [&](const SDL_FRect& rect) { damaged.push_back(rect); });
  return damaged;
}

bool Same(const SDL_FRect& a, const SDL_FRect& b) {
  return a.x == b.x && a.y == b.y && a.w == b.w && a.h == b.h;
}

// The mechanism, on its own: the first frame damages nothing (it paints whole),
// an unchanged frame nothing, a changed unit its rect, a moved one both rects, and
// one that appeared or went away its own rect.
void TestDerivedDamageRepaintsWhatChanged() {
  DerivedDamage damage;
  const SDL_FRect a{0, 0, 10, 10};
  const SDL_FRect b{10, 0, 10, 10};
  Expect(Commit(damage, {{a, 1}, {b, 2}}).empty(), "the first frame paints whole: no derived damage");
  Expect(Commit(damage, {{a, 1}, {b, 2}}).empty(), "nothing changed, nothing damaged");

  auto changed = Commit(damage, {{a, 1}, {b, 3}});
  Expect(changed.size() == 1 && Same(changed[0], b), "a changed unit damages its rect, and only it");

  const SDL_FRect b_moved{12, 0, 10, 10};
  auto moved = Commit(damage, {{a, 1}, {b_moved, 3}});
  Expect(moved.size() == 2 && Same(moved[0], b) && Same(moved[1], b_moved),
         "a unit that moved damages where it was and where it is");

  const SDL_FRect c{30, 0, 10, 10};
  auto appeared = Commit(damage, {{a, 1}, {b_moved, 3}, {c, 4}});
  Expect(appeared.size() == 1 && Same(appeared[0], c), "a unit that appeared damages its rect");
  auto gone = Commit(damage, {{a, 1}, {b_moved, 3}});
  Expect(gone.size() == 1 && Same(gone[0], c), "a unit that went away damages where it was");

  damage.Reset();
  Expect(Commit(damage, {{a, 9}}).empty(), "after a reset the next frame paints whole again");
}

// Every kind of value a unit folds in moves its fingerprint.
void TestPaintFingerprintFoldsEveryValue() {
  const auto base = [] {
    return PaintFingerprint().Add(std::string_view("Ln 1, Col 1")).Add(SDL_FRect{1, 2, 3, 4})
        .Add(SDL_Color{1, 2, 3, 4}).Add(true).Add(7);
  };
  const std::uint64_t reference = base().value();
  Expect(PaintFingerprint().Add(std::string_view("Ln 1, Col 2")).Add(SDL_FRect{1, 2, 3, 4})
                 .Add(SDL_Color{1, 2, 3, 4}).Add(true).Add(7).value() != reference, "text");
  Expect(PaintFingerprint().Add(std::string_view("Ln 1, Col 1")).Add(SDL_FRect{1, 2, 3, 5})
                 .Add(SDL_Color{1, 2, 3, 4}).Add(true).Add(7).value() != reference, "rect");
  Expect(PaintFingerprint().Add(std::string_view("Ln 1, Col 1")).Add(SDL_FRect{1, 2, 3, 4})
                 .Add(SDL_Color{1, 2, 3, 5}).Add(true).Add(7).value() != reference, "color");
  Expect(PaintFingerprint().Add(std::string_view("Ln 1, Col 1")).Add(SDL_FRect{1, 2, 3, 4})
                 .Add(SDL_Color{1, 2, 3, 4}).Add(false).Add(7).value() != reference, "flag");
  Expect(PaintFingerprint().Add(std::string_view("Ln 1, Col 1")).Add(SDL_FRect{1, 2, 3, 4})
                 .Add(SDL_Color{1, 2, 3, 4}).Add(true).Add(8).value() != reference, "integer");
  // Adjacent strings do not run together: "ab"+"c" is not "a"+"bc".
  Expect(PaintFingerprint().Add(std::string_view("ab")).Add(std::string_view("c")).value() !=
             PaintFingerprint().Add(std::string_view("a")).Add(std::string_view("bc")).value(),
         "string boundaries are part of the fingerprint");
}

// Every field a status-bar segment paints moves its fingerprint (and adding a
// field without folding it in does not compile: FingerprintOf binds them all).
void TestAStatusBarSegmentsFingerprintCoversWhatItPaints() {
  using Segment = workspace::PreparedFrameState::StatusBarSegmentPaint;
  const SDL_Color background{10, 10, 10, 255};
  const Segment base{.row = SDL_FRect{1, 2, 30, 18}, .text = "Ln 1, Col 1", .color = SDL_Color{200, 200, 200, 255},
                     .hovered = false};
  const std::uint64_t reference = workspace::FingerprintOf(base, background);
  Segment changed = base;
  changed.row.x = 2;
  Expect(workspace::FingerprintOf(changed, background) != reference, "the row");
  changed = base;
  changed.text = "Ln 1, Col 2";
  Expect(workspace::FingerprintOf(changed, background) != reference, "the text");
  changed = base;
  changed.color.r = 201;
  Expect(workspace::FingerprintOf(changed, background) != reference, "the color");
  changed = base;
  changed.hovered = true;
  Expect(workspace::FingerprintOf(changed, background) != reference, "the hover");
  Expect(workspace::FingerprintOf(base, SDL_Color{11, 10, 10, 255}) != reference, "the background");
}

}  // namespace

void RegisterDerivedDamageTests(std::vector<TestCase>& tests) {
  AddTest(tests, "DerivedDamage/RepaintsWhatChanged", TestDerivedDamageRepaintsWhatChanged);
  AddTest(tests, "DerivedDamage/PaintFingerprintFoldsEveryValue", TestPaintFingerprintFoldsEveryValue);
  AddTest(tests, "DerivedDamage/AStatusBarSegmentsFingerprintCoversWhatItPaints",
          TestAStatusBarSegmentsFingerprintCoversWhatItPaints);
}

}  // namespace microide::tests
