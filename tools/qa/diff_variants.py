"""Pairs of source files a structural diff has something to say about.

The compare view refines every hunk of the line diff with a structural diff
(src/compare/HunkRefinement.h): re-indenting, re-wrapping and re-spacing cost
nothing, wrapping code in a block marks only the new head and brace, a renamed
identifier marks the identifier, a replaced string is diffed word by word, and a
hunk too large for the search falls back to a flat token diff. The fixture's
files barely differ from one another, so the monkey's diffs never reached most
of that. These do.

`variant_pair(counter)` is deterministic in `counter`: a replay writes the same
two files. The base is a generated source in one of three languages (their
bracket and comment rules differ); the right side is the base with one to three
edits drawn from EDITS applied in order.
"""
from __future__ import annotations

import random
import re

LANGUAGES = ("cpp", "py", "js")

_NAMES = ("count", "total", "index", "buffer", "result", "value", "limit", "offset", "node", "entry")
_CALLS = ("compute", "update", "render", "flush", "merge", "parse", "resolve", "emit")
_WORDS = ("alpha", "beta", "gamma", "delta", "quick", "brown", "lazy", "fox")


def _base(rng: random.Random, language: str, functions: int) -> list[str]:
    lines: list[str] = []
    for f in range(functions):
        name = f"{rng.choice(_CALLS)}_{f}"
        a, b = rng.sample(_NAMES, 2)
        body = [
            f"{a} = {rng.choice(_CALLS)}({b}, {rng.randrange(100)}, \"{' '.join(rng.sample(_WORDS, 3))}\")",
            f"if {a} > {rng.randrange(50)}: {b} = {a} + {rng.randrange(9)}" if language == "py" else
            f"if ({a} > {rng.randrange(50)}) {{ {b} = {a} + {rng.randrange(9)}; }}",
            f"# {' '.join(rng.sample(_WORDS, 4))}" if language == "py" else
            f"// {' '.join(rng.sample(_WORDS, 4))}",
            f"return {rng.choice(_CALLS)}([{a}, {b}, {rng.randrange(1000)}])",
        ]
        if language == "py":
            lines.append(f"def {name}({a}, {b}):")
            lines.extend("    " + line for line in body)
        else:
            head = f"int {name}(int {a}, int {b}) {{" if language == "cpp" else f"function {name}({a}, {b}) {{"
            lines.append(head)
            lines.extend("  " + (line if line.startswith(("//", "if")) else line + ";") for line in body)
            lines.append("}")
        lines.append("")
    return lines


def _reindent(rng: random.Random, lines: list[str], language: str) -> list[str]:
    start = rng.randrange(len(lines))
    end = min(len(lines), start + rng.randrange(2, 12))
    extra = "\t" if rng.random() < 0.3 else " " * rng.choice((1, 2, 4))
    return lines[:start] + [(extra + line) if line.strip() else line for line in lines[start:end]] + lines[end:]


def _rewrap(rng: random.Random, lines: list[str], language: str) -> list[str]:
    """A call's arguments, one per line (or a wrapped call joined back)."""
    candidates = [i for i, line in enumerate(lines) if "(" in line and "," in line and line.rstrip().endswith((")", ");"))]
    if not candidates:
        return lines
    i = rng.choice(candidates)
    line = lines[i]
    indent = line[: len(line) - len(line.lstrip())]
    head, _, rest = line.partition("(")
    args, _, tail = rest.rpartition(")")
    parts = [p.strip() for p in args.split(",")]
    wrapped = [head + "("] + [indent + "    " + p + ("," if k < len(parts) - 1 else "") for k, p in enumerate(parts)]
    wrapped.append(indent + ")" + tail)
    return lines[:i] + wrapped + lines[i + 1:]


def _wrap_block(rng: random.Random, lines: list[str], language: str) -> list[str]:
    start = rng.randrange(len(lines))
    end = min(len(lines), start + rng.randrange(1, 6))
    inner = ["    " + line if line.strip() else line for line in lines[start:end]]
    if language == "py":
        return lines[:start] + ["if enabled:"] + inner + lines[end:]
    return lines[:start] + ["if (enabled) {"] + inner + ["}"] + lines[end:]


def _rename(rng: random.Random, lines: list[str], language: str) -> list[str]:
    old = rng.choice(_NAMES)
    new = old + rng.choice(("_v2", "Count", "_next"))
    pattern = re.compile(rf"\b{old}\b")
    return [pattern.sub(new, line) for line in lines]


def _edit_string(rng: random.Random, lines: list[str], language: str) -> list[str]:
    candidates = [i for i, line in enumerate(lines) if '"' in line]
    if not candidates:
        return lines
    i = rng.choice(candidates)
    word = rng.choice(_WORDS)
    return lines[:i] + [re.sub(r'"([^"]*)"', lambda m: f'"{m.group(1)} {word}"', lines[i], count=1)] + lines[i + 1:]


def _edit_comment(rng: random.Random, lines: list[str], language: str) -> list[str]:
    marker = "#" if language == "py" else "//"
    candidates = [i for i, line in enumerate(lines) if line.lstrip().startswith(marker)]
    if not candidates:
        return lines
    i = rng.choice(candidates)
    return lines[:i] + [lines[i] + " " + rng.choice(_WORDS)] + lines[i + 1:]


def _swap(rng: random.Random, lines: list[str], language: str) -> list[str]:
    if len(lines) < 3:
        return lines
    i = rng.randrange(len(lines) - 1)
    out = list(lines)
    out[i], out[i + 1] = out[i + 1], out[i]
    return out


def _insert_delete(rng: random.Random, lines: list[str], language: str) -> list[str]:
    i = rng.randrange(len(lines))
    if rng.random() < 0.5 and len(lines) > 4:
        return lines[:i] + lines[i + rng.randrange(1, 4):]
    marker = "#" if language == "py" else "//"
    return lines[:i] + [f"{marker} inserted {rng.choice(_WORDS)}", f"{rng.choice(_NAMES)} = {rng.randrange(99)}"] + lines[i:]


def _unbalance(rng: random.Random, lines: list[str], language: str) -> list[str]:
    """A delimiter that pairs with nothing: the lexer must fall back to punctuation."""
    i = rng.randrange(len(lines))
    return lines[:i] + [lines[i] + rng.choice(("(", ")", "{", "]", "\"", "/*"))] + lines[i + 1:]


def _scatter(rng: random.Random, lines: list[str], language: str) -> list[str]:
    """Many small changes far apart: many hunks, and one big one when they touch."""
    out = list(lines)
    for _ in range(rng.randrange(5, 30)):
        i = rng.randrange(len(out))
        out[i] = re.sub(r"\d+", lambda m: str(int(m.group(0)) + 1), out[i], count=1)
    return out


EDITS = (_reindent, _rewrap, _wrap_block, _rename, _edit_string, _edit_comment, _swap, _insert_delete,
         _unbalance, _scatter)


def variant_pair(counter: int) -> tuple[str, str, str]:
    """(language extension, left text, right text) for `counter`."""
    rng = random.Random(0x5D1FF ^ counter)
    language = LANGUAGES[counter % len(LANGUAGES)]
    # Mostly small files; now and then one large enough that a hunk outgrows the
    # structural search's budget and takes the flat token diff.
    functions = rng.choice((2, 4, 8, 16, 200)) if rng.random() < 0.9 else 400
    left = _base(rng, language, functions)
    right = list(left)
    for _ in range(rng.randrange(1, 4)):
        right = rng.choice(EDITS)(rng, right, language) or right
    return language, "\n".join(left) + "\n", "\n".join(right) + "\n"
