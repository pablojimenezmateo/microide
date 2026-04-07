# Fixture Corpus

These fixtures are meant to be consumed by future unit, integration, and golden tests.

- `large/plain/large_story.txt` is a large plain-text file with predictable anchors, long lines, tabs, and repeated search tokens.
- `large/code/large_sample.cpp` is a large C++ translation unit with comments, macros, strings, raw strings, templates, numbers, and repeated function bodies.
- `syntax/cpp_stress.cpp` is a smaller syntax-focused C++ file for token-category snapshots.
- `syntax/unified_diff.patch` is a patch fixture for diff-file syntax highlighting.
- `diff/simple/` contains a deterministic text pair with stable expected row and hunk counts.
- `diff/code/` contains a code-oriented compare pair for visual diff and syntax-in-compare tests.
- `diff/git/base/` and `diff/git/head/` are matching working trees intended for tests that create a temporary git repository and exercise file-history and compare flows.

`manifest.json` records the generated files, their sizes, and exact expectations for the deterministic diff pairs.
