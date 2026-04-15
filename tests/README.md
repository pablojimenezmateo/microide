# Microide Test Assets

This directory is the seed for future automated coverage in the repo-root C++ project.

- `fixtures/` contains committed sample inputs for large-file, syntax, and diff workflows.
- `generate_fixtures.py` regenerates the fixture corpus and `fixtures/manifest.json`.

The fixture set is intentionally biased toward current migration risks:

- large plain-text scrolling and search
- large code-file loading plus syntax highlighting
- diff row and hunk mapping
- temporary-git-repo setup for compare workflow tests

Regenerate the corpus with:

```bash
python3 tests/generate_fixtures.py
```

Run the first automated checks with:

```bash
cmake -S . -B build/microide
cmake --build build/microide
ctest --test-dir build/microide --output-on-failure
```

Run a focused subset of the in-tree test binary with one or more substring filters:

```bash
./build/microide/microide_tests TextRenderer
./build/microide/microide_tests "WorkspaceShell/EditorDirty"
```
