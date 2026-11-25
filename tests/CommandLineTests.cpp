#include "TestSupport.h"

#include "util/CommandLine.h"

#include <string>
#include <vector>

namespace microide::tests {
namespace {

using microide::util::SplitCommandLine;

std::string Show(const std::vector<std::string>& words) {
  std::string text = "[";
  for (std::size_t i = 0; i < words.size(); ++i) {
    if (i != 0) {
      text += ", ";
    }
    text += '"';
    text += words[i];
    text += '"';
  }
  text += ']';
  return text;
}

void ExpectSplit(std::string_view input, const std::vector<std::string>& expected) {
  const std::vector<std::string> actual = SplitCommandLine(input);
  Expect(actual == expected,
         "SplitCommandLine(\"" + std::string(input) + "\") should be " + Show(expected) +
             ", got " + Show(actual));
}

void TestSplitCommandLineWords() {
  ExpectSplit("", {});
  ExpectSplit("   ", {});
  ExpectSplit("bash", {"bash"});
  ExpectSplit("  /bin/zsh  ", {"/bin/zsh"});
  ExpectSplit("ssh build-host", {"ssh", "build-host"});
  ExpectSplit("ssh -t host tmux new -A", {"ssh", "-t", "host", "tmux", "new", "-A"});
  ExpectSplit("a\tb\nc", {"a", "b", "c"});
}

void TestSplitCommandLineQuoting() {
  ExpectSplit("\"one word\"", {"one word"});
  ExpectSplit("'one word'", {"one word"});
  ExpectSplit("ssh host \"cd /a b && exec bash\"", {"ssh", "host", "cd /a b && exec bash"});
  ExpectSplit("pre\"fix\"post", {"prefixpost"});
  ExpectSplit("'a\\\"b'", {"a\\\"b"});
  ExpectSplit("\"a\\\"b\"", {"a\"b"});
  ExpectSplit("\"a\\nb\"", {"a\\nb"});
  ExpectSplit("/opt/my\\ shell/bin/sh", {"/opt/my shell/bin/sh"});
  ExpectSplit("ssh \"host", {"ssh", "host"});
  ExpectSplit("ssh 'host", {"ssh", "host"});
}

}

void RegisterCommandLineTests(std::vector<TestCase>& tests) {
  AddTest(tests, "CommandLine/SplitWords", TestSplitCommandLineWords);
  AddTest(tests, "CommandLine/SplitQuoting", TestSplitCommandLineQuoting);
}

}
