# Resolved debt ledger — moved 2026-11-25

Moved from `dev-docs/project/known-tech-debt.md`'s open queue as each entry was
resolved or closed, with its resolution appended. Newest first.

### TD-2026-11-11-440 — the structural diff reads a lexer's tree, not a grammar's. [CLOSED 2026-11-25 (won't do)]

The compare view's structural refinement (`src/compare/DiffSyntax.*`,
`StructuralDiff.*`, openspec `structural-diff`) builds each hunk side's syntax
forest from a language-aware lexer plus bracket matching, with the rules
(`DiffSyntax`) derived from the language contract: comment tokens and bracket
pairs. difftastic parses with tree-sitter, so its lists are real syntax nodes --
an `if` owns its condition and its block, a call owns its callee. Here those are
siblings inside the enclosing brackets, which makes some matches coarser than
difftastic's: a novel `if (x) {` head is three atoms and a list rather than one
node, and the graph search cannot prefer "the same statement, edited" over "a
different statement" beyond what bracket nesting says.

Deliberately not taken now: a grammar per language is a C dependency plus one
generated parser per language (several MB each), and the lexer covers every
language the editor knows. The seam is ready for it: `BuildDiffSyntaxTree` is the
only producer of a `DiffSyntaxTree`, and the search reads nothing but nodes,
tokens and content ids. A grammar-backed builder for the top languages, with the
lexer kept as the fallback, is the follow-up; gate it on `microide_diff_bench`.

**Closed 2026-11-25, won't do** (the user's call): no grammar dependency. The lexer covers every language the editor knows at no per-language binary cost, and the seam stays where this entry put it -- `BuildDiffSyntaxTree` is the only producer of a `DiffSyntaxTree` -- should a grammar-backed builder ever be wanted.

### TD-2026-11-13-449 — SDL's X11 clipboard can paste the previous text right after another application copies. [CLOSED 2026-11-25 (upstream, not reported)]

Found standing up the QA driver's foreign clipboard (`App.foreign_clipboard`,
`tools/qa/x_clipboard.py`): a paste a few milliseconds after another client took
the CLIPBOARD returned the text of the paste BEFORE it, into a buffer confirmed
empty -- about one paste in three in `edit-foreign-clipboard` without a settle.

The mechanism is SDL 3.2's X11 backend (`SDL_x11clipboard.c` / `SDL_x11events.c`):
on the XFixes notice that the owner changed, SDL asks the new owner for TARGETS
(into the `SDL_FORMATS` property). A paste then runs `GetSelectionData`, which
converts into the `SDL_SELECTION` property and waits on `selection_waiting` --
a flag ANY SelectionNotify clears, the TARGETS answer included. It then reads
`SDL_SELECTION`, which still holds the previous paste (read with delete=False),
and returns it. The same read also happens after its 1 s timeout, where it then
takes the selection itself with empty data.

Who meets it: a paste right after a copy elsewhere, and more often with a
clipboard manager, which re-takes the selection after every copy. The app cannot
tell: `SystemClipboard::Read` gets well-formed text. The fix is SDL's: match the
SelectionNotify to the request (its target and property), and delete or clear
the property before converting. Report upstream; the QA scenario settles 0.5 s
before pasting meanwhile, as a person does.

**Closed 2026-11-25** (the user's call): no upstream report from here. Nothing in the app can tell the stale text from a fresh paste, so there is nothing to fix in this tree; the QA scenario keeps its 0.5 s settle. Reopen if a user meets it, with the mechanism above as the report.

### TD-2026-11-16-451 — the rr on the soak machine is patched for tee(2); upstream is not. [CLOSED 2026-11-25 (patch kept local)]

`rr record` aborts the whole recording -- the app with it -- at a system call
it does not know ("Expected ENOSYS for 'tee' ... unsupported by rr",
`src/record_syscall.cc`). Ubuntu's coreutils (`cat`, `head`, `seq`, `yes`)
call `tee` on pipes, so a recorded soak job lost its recorder the first time a
terminal command or a task ran one: twice in 500 steps of the `terminal`
profile, four times in 240 of `world` (2026-11-16, rr 5.10.0 at a808942).

Fixed here, not upstream: `tools/soak/rr-tee.patch` (twelve lines: `tee` writes
no user memory, so it is an `IrregularEmulatedSyscall` that may switch, beside
`splice`) is applied to `build/rr-src` and installed to `/usr/local/bin/rr`.
Rebuilding rr from a fresh checkout loses it: `git apply` the patch first, then
`cmake --build obj && sudo cmake --install obj`, then `chmod a+rX` the
installed files (the install runs under umask 0077).

Still open: send the patch upstream, and until a release carries it, a machine
with a stock rr lists `terminal` and `world` in `soak.py`'s
`UNRECORDED_PROFILES`. A run that loses its recorder to any other system call
reports a `recorder` finding the ledger does not book; `soak.py health` counts
them, and that count is the signal to look for the next one.

**Closed 2026-11-25** (the user's call): the patch is not sent upstream. It stays in `tools/soak/rr-tee.patch` with the rebuild steps above, and a machine with a stock rr lists `terminal` and `world` in `UNRECORDED_PROFILES`; `soak.py health`'s `recorder` count remains the signal for the next unsupported call.
