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
  // The case the whole change exists for: a shell setting that is a command line.
  ExpectSplit("ssh build-host", {"ssh", "build-host"});
  ExpectSplit("ssh -t host tmux new -A", {"ssh", "-t", "host", "tmux", "new", "-A"});
  // Tabs and newlines separate words too; a config file can carry either.
  ExpectSplit("a\tb\nc", {"a", "b", "c"});
}

void TestSplitCommandLineQuoting() {
  ExpectSplit("\"one word\"", {"one word"});
  ExpectSplit("'one word'", {"one word"});
  ExpectSplit("ssh host \"cd /a b && exec bash\"", {"ssh", "host", "cd /a b && exec bash"});
  // Quotes join rather than separate when they abut: pre"fix"post is one word.
  ExpectSplit("pre\"fix\"post", {"prefixpost"});
  // Single quotes are literal: no escape processing inside them at all.
  ExpectSplit("'a\\\"b'", {"a\\\"b"});
  // Double quotes process a backslash only before " \\ and $.
  ExpectSplit("\"a\\\"b\"", {"a\"b"});
  ExpectSplit("\"a\\nb\"", {"a\\nb"});
  // A bare backslash escapes the next character, including a space.
  ExpectSplit("/opt/my\\ shell/bin/sh", {"/opt/my shell/bin/sh"});
  // An unterminated quote yields the partial word rather than nothing: the useful
  // failure is "microide tried to run this and could not", not a setting that
  // silently reads as empty and starts the default shell as if nothing were set.
  ExpectSplit("ssh \"host", {"ssh", "host"});
  ExpectSplit("ssh 'host", {"ssh", "host"});
}

}  // namespace

void RegisterCommandLineTests(std::vector<TestCase>& tests) {
  AddTest(tests, "CommandLine/SplitWords", TestSplitCommandLineWords);
  AddTest(tests, "CommandLine/SplitQuoting", TestSplitCommandLineQuoting);
}

}  // namespace microide::tests
