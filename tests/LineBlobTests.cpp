#include "TestSupport.h"

#include "editor/LineBlob.h"

#include <cstddef>
#include <cstdint>
#include <random>
#include <string>
#include <vector>

namespace microide::tests {
namespace {

using microide::editor::LineBlob;

std::vector<std::string> RandomLines(std::mt19937& rng, std::size_t max_lines) {
  std::uniform_int_distribution<std::size_t> count(0, max_lines);
  std::uniform_int_distribution<int> length(0, 12);
  std::uniform_int_distribution<int> letter('a', 'z');
  std::vector<std::string> lines(count(rng));
  for (std::string& line : lines) {
    line.resize(static_cast<std::size_t>(length(rng)));
    for (char& c : line) {
      c = static_cast<char>(letter(rng));
    }
  }
  return lines;
}

void ExpectMatches(const LineBlob& blob, const std::vector<std::string>& model, int step) {
  const std::string at = " (step " + std::to_string(step) + ")";
  Expect(blob.size() == model.size(), "line count matches the model" + at);
  std::size_t bytes = 0;
  for (std::size_t i = 0; i < model.size() && i < blob.size(); ++i) {
    Expect(blob[i] == model[i], "line " + std::to_string(i) + " matches the model" + at);
    bytes += model[i].size();
  }
  Expect(blob.content_bytes() == bytes, "content bytes match the model" + at);
  Expect(blob == LineBlob(model), "equal to a blob built straight from the model" + at);
  Expect(blob.ToVector() == model, "ToVector reproduces the model" + at);
}

// The undo group's aggregate is prepended to once per region of a multi-region
// edit, and prepending used to shift the whole blob each time (TD-2026-09-20-301).
// The blob now keeps front slack; every operation has to keep meaning the same
// lines however much slack sits in front of them.
void TestLineBlobMatchesAVectorModelUnderEveryOperation() {
  std::mt19937 rng(20261026);
  LineBlob blob;
  std::vector<std::string> model;
  std::uniform_int_distribution<int> op(0, 5);
  for (int step = 0; step < 4000; ++step) {
    const std::vector<std::string> lines = RandomLines(rng, 4);
    switch (op(rng)) {
      case 0:
      case 1:
        blob.prepend(LineBlob(lines));
        model.insert(model.begin(), lines.begin(), lines.end());
        break;
      case 2:
        blob.append(LineBlob(lines));
        model.insert(model.end(), lines.begin(), lines.end());
        break;
      case 3:
        for (const std::string& line : lines) {
          blob.push_back(line);
          model.push_back(line);
        }
        break;
      case 4: {
        std::uniform_int_distribution<std::size_t> at(0, model.size());
        std::size_t first = at(rng);
        std::size_t last = at(rng);
        if (first > last) {
          std::swap(first, last);
        }
        blob.replace_range(first, last, LineBlob(lines));
        model.erase(model.begin() + static_cast<std::ptrdiff_t>(first),
                    model.begin() + static_cast<std::ptrdiff_t>(last));
        model.insert(model.begin() + static_cast<std::ptrdiff_t>(first), lines.begin(), lines.end());
        break;
      }
      default: {
        std::uniform_int_distribution<std::size_t> at(0, model.size());
        std::size_t first = at(rng);
        std::size_t last = at(rng);
        if (first > last) {
          std::swap(first, last);
        }
        const LineBlob slice = blob.SubRange(first, last);
        const std::vector<std::string> expected(model.begin() + static_cast<std::ptrdiff_t>(first),
                                                model.begin() + static_cast<std::ptrdiff_t>(last));
        ExpectMatches(slice, expected, step);
        break;
      }
    }
    if (model.size() > 300) {
      blob.clear();
      model.clear();
    }
    ExpectMatches(blob, model, step);
  }
  // Prepending a blob to itself doubles it.
  blob = LineBlob::Of({"x", "y"});
  blob.prepend(blob);
  ExpectMatches(blob, {"x", "y", "x", "y"}, -1);
}

// What the slack is for: a prepend writes in front of the content and moves none
// of it, except on the O(log N) prepends that regrow the buffers -- so N
// prepends move O(N) bytes rather than O(N^2). The old in place insert also
// regrew only logarithmically; what it paid was shifting every byte on every
// call, which is exactly what the address of the last line shows.
void TestLineBlobPrependDoesNotMoveTheContent() {
  LineBlob blob = LineBlob::Of({"seed"});
  constexpr int kPrepends = 10000;
  const LineBlob line = LineBlob::Of({"a line of text"});
  int moves = 0;
  const char* seed_bytes = blob.back().data();
  for (int i = 0; i < kPrepends; ++i) {
    blob.prepend(line);
    if (blob.back().data() != seed_bytes) {
      ++moves;
      seed_bytes = blob.back().data();
    }
  }
  Expect(blob.size() == kPrepends + 1 && blob.back() == "seed" && blob.front() == "a line of text",
         "the prepends land in order");
  Expect(moves <= 40, "10,000 prepends should move the existing content a logarithmic number of "
                      "times, moved it " + std::to_string(moves));
}

}  // namespace

void RegisterLineBlobTests(std::vector<TestCase>& tests) {
  AddTest(tests, "LineBlob/MatchesAVectorModelUnderEveryOperation",
          TestLineBlobMatchesAVectorModelUnderEveryOperation);
  AddTest(tests, "LineBlob/PrependDoesNotMoveTheContent", TestLineBlobPrependDoesNotMoveTheContent);
}

}  // namespace microide::tests
