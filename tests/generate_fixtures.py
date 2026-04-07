#!/usr/bin/env python3

from __future__ import annotations

import json
from dataclasses import dataclass
from pathlib import Path
import textwrap


ROOT = Path(__file__).resolve().parent
FIXTURES_DIR = ROOT / "fixtures"


def write_fixture(relative_path: str, content: str) -> dict[str, object]:
    path = FIXTURES_DIR / relative_path
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(content, encoding="utf-8")
    lines = split_compare_lines(content)
    longest_line = max((len(line) for line in lines), default=0)
    return {
        "path": relative_path,
        "bytes": len(content.encode("utf-8")),
        "line_count": len(lines),
        "longest_line": longest_line,
    }


def split_compare_lines(text: str) -> list[str]:
    lines: list[str] = []
    start = 0
    while start < len(text):
        newline = text.find("\n", start)
        if newline == -1:
            lines.append(text[start:])
            break
        lines.append(text[start:newline])
        start = newline + 1
    return lines


@dataclass
class DiffRow:
    left_line: int
    right_line: int
    kind: str


def build_compare_summary(left: str, right: str) -> dict[str, int]:
    left_lines = split_compare_lines(left)
    right_lines = split_compare_lines(right)
    left_count = len(left_lines)
    right_count = len(right_lines)
    dp = [0] * ((left_count + 1) * (right_count + 1))

    def at(i: int, j: int) -> int:
        return dp[i * (right_count + 1) + j]

    def set_at(i: int, j: int, value: int) -> None:
        dp[i * (right_count + 1) + j] = value

    for i in range(left_count - 1, -1, -1):
        for j in range(right_count - 1, -1, -1):
            if left_lines[i] == right_lines[j]:
                set_at(i, j, at(i + 1, j + 1) + 1)
            else:
                set_at(i, j, max(at(i + 1, j), at(i, j + 1)))

    ops: list[tuple[str, str]] = []
    i = 0
    j = 0
    while i < left_count and j < right_count:
        if left_lines[i] == right_lines[j]:
            ops.append(("equal", left_lines[i]))
            i += 1
            j += 1
        elif at(i + 1, j) >= at(i, j + 1):
            ops.append(("delete", left_lines[i]))
            i += 1
        else:
            ops.append(("insert", right_lines[j]))
            j += 1
    while i < left_count:
        ops.append(("delete", left_lines[i]))
        i += 1
    while j < right_count:
        ops.append(("insert", right_lines[j]))
        j += 1

    rows: list[DiffRow] = []
    hunks = 0
    left_line = 1
    right_line = 1
    op_index = 0
    while op_index < len(ops):
        kind, _ = ops[op_index]
        if kind == "equal":
            rows.append(DiffRow(left_line=left_line, right_line=right_line, kind="unchanged"))
            left_line += 1
            right_line += 1
            op_index += 1
            continue

        hunks += 1
        deleted_lines: list[str] = []
        inserted_lines: list[str] = []
        while op_index < len(ops) and ops[op_index][0] != "equal":
            op_kind, text = ops[op_index]
            if op_kind == "delete":
                deleted_lines.append(text)
            elif op_kind == "insert":
                inserted_lines.append(text)
            op_index += 1

        row_count = max(len(deleted_lines), len(inserted_lines))
        for row_index in range(row_count):
            row_left_line = 0
            row_right_line = 0
            if row_index < len(deleted_lines):
                row_left_line = left_line
                left_line += 1
            if row_index < len(inserted_lines):
                row_right_line = right_line
                right_line += 1
            if row_left_line and row_right_line:
                row_kind = "modified"
            elif row_left_line:
                row_kind = "deleted"
            else:
                row_kind = "added"
            rows.append(DiffRow(left_line=row_left_line, right_line=row_right_line, kind=row_kind))

    summary = {
        "hunks": hunks,
        "rows": len(rows),
        "unchanged": 0,
        "modified": 0,
        "added": 0,
        "deleted": 0,
    }
    for row in rows:
        summary[row.kind] += 1
    return summary


def generate_large_story() -> str:
    lines = [
        "fixture large story",
        "purpose: plain text loading, search, scroll, selection, and long-line behavior",
        "format: stable generated content with periodic anchors",
    ]
    for index in range(1, 4097):
        line = (
            f"entry {index:04d} bucket={index % 29:02d} anchor={index % 97:02d} "
            f"token=plain_{index % 13:02d} marker=scan_{index % 41:02d} "
            f"text=the_quick_brown_fox_jumps_over_the_lazy_dog"
        )
        if index % 64 == 0:
            line += (
                f"\tcolumns\tleft={index - 3:04d}\tmiddle={index:04d}\tright={index + 3:04d}"
            )
        if index % 128 == 0:
            segments = " ".join(f"segment_{part:02d}=value_{index + part:04d}" for part in range(12))
            line += f" long_line={segments}"
        if index % 257 == 0:
            line += f" search_target=NEEDLE_{index:04d}"
        lines.append(line)
    return "\n".join(lines) + "\n"


def generate_large_cpp() -> str:
    lines = [
        "#include <algorithm>",
        "#include <array>",
        "#include <cstdint>",
        "#include <optional>",
        "#include <sstream>",
        "#include <string>",
        "#include <string_view>",
        "#include <utility>",
        "#include <vector>",
        "",
        "#define FIXTURE_TRACE(label) do { (void)(label); } while (false)",
        "",
        "namespace microide::fixtures {",
        "",
        "struct Record {",
        "  int id = 0;",
        "  std::string label;",
        "  double weight = 0.0;",
        "  bool active = false;",
        "};",
        "",
        "template <typename T>",
        "constexpr T ClampValue(T value, T lower, T upper) {",
        "  return value < lower ? lower : (value > upper ? upper : value);",
        "}",
        "",
        "constexpr std::string_view kBanner = R\"FIXTURE(",
        "microide generated fixture",
        "with a raw string for syntax tests",
        ")FIXTURE\";",
    ]
    for block in range(1, 181):
        lines.extend(
            [
                "",
                f"// block {block:03d} keeps structure repetitive on purpose for large-file validation.",
                f"constexpr std::array<int, 6> kBlock{block:03d} = "
                f"{{{block}, {block + 1}, {block + 2}, {block + 3}, {block + 4}, {block + 5}}};",
                "",
                f"Record MakeRecord{block:03d}(std::string_view seed, int salt) {{",
                f"  const double scale = static_cast<double>(({block} % 7) + 1) / 3.0;",
                f"  const bool active = ((salt + {block}) % 2) == 0;",
                f"  return Record{{",
                f"      .id = salt + {block},",
                "      .label = std::string(seed) + \"-block-" + f"{block:03d}" + "\",",
                "      .weight = ClampValue(scale * static_cast<double>(salt + 1), 0.0, 4096.0),",
                "      .active = active,",
                "  };",
                "}",
                "",
                f"int AccumulateBlock{block:03d}(const std::vector<Record>& records) {{",
                "  int total = 0;",
                "  for (const Record& record : records) {",
                f"    if ((record.id + {block}) % 3 == 0) {{",
                f"      total += record.id + static_cast<int>(record.weight) + {block};",
                "    } else {",
                f"      total -= ({block} % 5);",
                "    }",
                "  }",
                "  return total;",
                "}",
                "",
                f"std::string DescribeBlock{block:03d}(const Record& record) {{",
                "  std::ostringstream stream;",
                f"  stream << \"block={block:03d}\"",
                "         << \" id=\" << record.id",
                "         << \" label=\" << record.label",
                "         << \" weight=\" << record.weight",
                "         << \" active=\" << (record.active ? \"true\" : \"false\");",
                "  return stream.str();",
                "}",
            ]
        )
        if block % 15 == 0:
            lines.extend(
                [
                    "",
                    "/*",
                    "  Multi-line comment block inserted periodically so syntax tests see",
                    "  long comment regions amid otherwise repetitive code.",
                    "*/",
                    f"std::optional<Record> FindSpecialRecord{block:03d}(const std::vector<Record>& records) {{",
                    "  auto it = std::find_if(records.begin(), records.end(), [](const Record& record) {",
                    f"    return record.active && (record.id % {block // 3}) == 0;",
                    "  });",
                    "  if (it == records.end()) {",
                    "    return std::nullopt;",
                    "  }",
                    "  return *it;",
                    "}",
                ]
            )
    lines.extend(
        [
            "",
            "}  // namespace microide::fixtures",
            "",
            "int main() {",
            "  using microide::fixtures::Record;",
            "  std::vector<Record> records;",
            "  records.push_back(microide::fixtures::MakeRecord001(\"alpha\", 1));",
            "  records.push_back(microide::fixtures::MakeRecord090(\"beta\", 2));",
            "  records.push_back(microide::fixtures::MakeRecord180(\"gamma\", 3));",
            "  const int score = microide::fixtures::AccumulateBlock090(records);",
            "  FIXTURE_TRACE(score);",
            "  return score == 0 ? 1 : 0;",
            "}",
        ]
    )
    return "\n".join(lines) + "\n"


def generate_cpp_syntax_stress() -> str:
    return textwrap.dedent(
        """\
        #include <array>
        #include <concepts>
        #include <optional>
        #include <string>
        #include <string_view>
        #include <vector>

        #define MICROIDE_TEST_FLAG(name) "flag:" name
        #define MICROIDE_JOIN(a, b) a##b

        namespace microide::syntax_fixture {

        template <typename T>
        concept NumberLike = std::integral<T> || std::floating_point<T>;

        struct Widget {
          int id = 7;
          std::string name = "widget";
          bool enabled = true;
        };

        constexpr std::string_view kRaw = R"CPP(
        line one
        line two with "quotes" and // comment text inside the string
        )CPP";

        template <NumberLike T>
        constexpr T Blend(T left, T right) {
          return left + (right - left) / static_cast<T>(2);
        }

        std::optional<std::string> FormatWidget(const Widget& widget) {
          // Single-line comment with 1234 and 0xff values.
          if (!widget.enabled) {
            return std::nullopt;
          }

          /*
            Multi-line comment with punctuation, braces {}, and operators ++ --.
          */
          const auto label = [name = widget.name, id = widget.id]() -> std::string {
            return name + ":" + std::to_string(id);
          };

          const double mixed = Blend(1.5, 9.5);
          const char marker = '\\n';
          return label() + "|" + std::to_string(mixed) + "|" + marker;
        }

        }  // namespace microide::syntax_fixture
        """
    )


def generate_patch_fixture() -> str:
    return textwrap.dedent(
        """\
        diff --git a/src/editor/Viewport.cpp b/src/editor/Viewport.cpp
        index 1234567..89abcde 100644
        --- a/src/editor/Viewport.cpp
        +++ b/src/editor/Viewport.cpp
        @@ -10,7 +10,8 @@ void Viewport::ScrollToCursor() {
        -  scroll_line_ = cursor_line_;
        +  scroll_line_ = ClampLine(cursor_line_);
        +  horizontal_scroll_ = 0;
           EnsureCursorVisible();
         }

        diff --git a/src/workspace/StatusBar.cpp b/src/workspace/StatusBar.cpp
        index 1111111..2222222 100644
        --- a/src/workspace/StatusBar.cpp
        +++ b/src/workspace/StatusBar.cpp
        @@ -20,6 +20,7 @@ void StatusBar::Render() {
           DrawEncoding();
           DrawLineEnding();
        +  DrawFileType();
           DrawCursorPosition();
         }
        """
    )


def generate_simple_diff_pair() -> tuple[str, str]:
    left = "\n".join(
        [
            "anchor-A-keep",
            "anchor-B-old",
            "anchor-C-keep",
            "anchor-D-old-1",
            "anchor-D-old-2",
            "anchor-E-keep",
            "anchor-F-delete-only",
            "anchor-G-keep",
        ]
    ) + "\n"
    right = "\n".join(
        [
            "anchor-A-keep",
            "anchor-B-new",
            "anchor-C-keep",
            "anchor-D-new-1",
            "anchor-D-new-2",
            "anchor-D-new-3",
            "anchor-E-keep",
            "anchor-G-keep",
            "anchor-H-add-only",
        ]
    ) + "\n"
    return left, right


def generate_code_diff_pair() -> tuple[str, str]:
    before = textwrap.dedent(
        """\
        #include <string>
        #include <vector>

        namespace microide::diff_fixture {

        struct StatusEntry {
          std::string label;
          int count = 0;
        };

        std::vector<StatusEntry> BuildEntries() {
          return {
              {"open", 3},
              {"saved", 7},
              {"closed", 1},
          };
        }

        int CountVisible(const std::vector<StatusEntry>& entries) {
          int total = 0;
          for (const StatusEntry& entry : entries) {
            if (entry.count > 0) {
              total += entry.count;
            }
          }
          return total;
        }

        std::string RenderSummary() {
          const auto entries = BuildEntries();
          return "visible=" + std::to_string(CountVisible(entries));
        }

        }  // namespace microide::diff_fixture
        """
    )
    after = textwrap.dedent(
        """\
        #include <string>
        #include <vector>

        namespace microide::diff_fixture {

        struct StatusEntry {
          std::string label;
          int count = 0;
          bool pinned = false;
        };

        std::vector<StatusEntry> BuildEntries() {
          return {
              {"open", 3, true},
              {"saved", 8, false},
              {"staged", 2, true},
          };
        }

        int CountVisible(const std::vector<StatusEntry>& entries) {
          int total = 0;
          for (const StatusEntry& entry : entries) {
            if (entry.count > 0 && !entry.label.empty()) {
              total += entry.count;
            }
          }
          return total;
        }

        std::string RenderSummary() {
          const auto entries = BuildEntries();
          return "visible=" + std::to_string(CountVisible(entries)) + ",first=" + entries.front().label;
        }

        }  // namespace microide::diff_fixture
        """
    )
    return before, after


def generate_git_seed_pair() -> tuple[dict[str, str], dict[str, str]]:
    base = {
        "README.md": "# diff repo seed\n\nThis tree is used to initialize a temporary git repository in tests.\n",
        "src/session.cpp": textwrap.dedent(
            """\
            #include <string>

            namespace microide::seed {

            std::string ActivePaneLabel(bool terminal_focused) {
              return terminal_focused ? "terminal" : "editor";
            }

            int VisiblePanelCount(bool sidebar_open, bool terminal_open) {
              int count = 1;
              if (sidebar_open) {
                ++count;
              }
              if (terminal_open) {
                ++count;
              }
              return count;
            }

            }  // namespace microide::seed
            """
        ),
    }
    head = {
        "README.md": "# diff repo seed\n\nThis tree is used to initialize a temporary git repository in tests.\n",
        "src/session.cpp": textwrap.dedent(
            """\
            #include <string>

            namespace microide::seed {

            std::string ActivePaneLabel(bool terminal_focused, bool compare_focused) {
              if (compare_focused) {
                return "compare";
              }
              return terminal_focused ? "terminal" : "editor";
            }

            int VisiblePanelCount(bool sidebar_open, bool terminal_open, bool log_open) {
              int count = 1;
              if (sidebar_open) {
                ++count;
              }
              if (terminal_open) {
                ++count;
              }
              if (log_open) {
                ++count;
              }
              return count;
            }

            }  // namespace microide::seed
            """
        ),
        "src/new_panel.cpp": textwrap.dedent(
            """\
            #include <string_view>

            namespace microide::seed {

            std::string_view DefaultPanelName() {
              return "problems";
            }

            }  // namespace microide::seed
            """
        ),
    }
    return base, head


def main() -> None:
    manifest: dict[str, object] = {
        "version": 1,
        "notes": [
            "Fixtures are generated and committed so tests can use stable paths.",
            "Diff expectations are computed with the same line-oriented semantics as CompareModel.cpp.",
        ],
    }

    large_story = generate_large_story()
    large_cpp = generate_large_cpp()
    syntax_cpp = generate_cpp_syntax_stress()
    syntax_patch = generate_patch_fixture()
    simple_left, simple_right = generate_simple_diff_pair()
    code_before, code_after = generate_code_diff_pair()
    git_base, git_head = generate_git_seed_pair()

    manifest["large_files"] = {
        "plain": {
            **write_fixture("large/plain/large_story.txt", large_story),
            "search_tokens": ["NEEDLE_0257", "NEEDLE_1285", "NEEDLE_3855"],
            "notable_lines": [1, 64, 128, 512, 1024, 2048, 4096],
        },
        "code": {
            **write_fixture("large/code/large_sample.cpp", large_cpp),
            "language": "cpp",
            "notable_blocks": [1, 15, 90, 180],
        },
    }

    manifest["syntax_cases"] = [
        {
            **write_fixture("syntax/cpp_stress.cpp", syntax_cpp),
            "language": "cpp",
            "expected_categories": [
                "preprocessor",
                "comment",
                "string",
                "number",
                "type",
                "statement",
                "operator",
            ],
        },
        {
            **write_fixture("syntax/unified_diff.patch", syntax_patch),
            "language": "patch",
            "expected_categories": ["diff-header", "context", "insert", "delete"],
        },
    ]

    manifest["diff_cases"] = [
        {
            "name": "simple-text",
            "left": write_fixture("diff/simple/left.txt", simple_left)["path"],
            "right": write_fixture("diff/simple/right.txt", simple_right)["path"],
            "expected": build_compare_summary(simple_left, simple_right),
        },
        {
            "name": "code-compare",
            "left": write_fixture("diff/code/before.cpp", code_before)["path"],
            "right": write_fixture("diff/code/after.cpp", code_after)["path"],
            "expected": build_compare_summary(code_before, code_after),
        },
    ]

    for relative_path, content in git_base.items():
        write_fixture(f"diff/git/base/{relative_path}", content)
    for relative_path, content in git_head.items():
        write_fixture(f"diff/git/head/{relative_path}", content)

    manifest["git_repo_seed"] = {
        "base_dir": "diff/git/base",
        "head_dir": "diff/git/head",
        "tracked_files": ["src/session.cpp"],
        "new_files": ["src/new_panel.cpp"],
        "session_compare_expected": build_compare_summary(
            git_base["src/session.cpp"], git_head["src/session.cpp"]
        ),
    }

    manifest_path = FIXTURES_DIR / "manifest.json"
    manifest_path.write_text(json.dumps(manifest, indent=2, sort_keys=True) + "\n", encoding="utf-8")


if __name__ == "__main__":
    main()
