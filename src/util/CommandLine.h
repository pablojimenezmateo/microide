#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace microide::util {

// Split a command line into argv words the way a POSIX shell's word splitting does
// for the simple cases: whitespace separates words, single quotes take everything
// literally, double quotes take everything literally except a backslash before `"`,
// `\` or `$`, and a bare backslash escapes the next character.
//
// It deliberately does NOT expand variables, globs, `~`, or anything else — a setting
// that names a program and its arguments wants quoting, not a shell. A caller that
// wants a shell runs one (`<shell> -lc <text>`); this is for the other case, where
// the text IS the argv.
//
// An unterminated quote yields the partial word rather than an error: the value comes
// from a hand-edited config file, and the useful failure is "microide tried to run
// this and could not", not a setting that silently reads as empty.
std::vector<std::string> SplitCommandLine(std::string_view text);

}  // namespace microide::util
