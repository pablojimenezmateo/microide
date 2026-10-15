# tools/qa — drive the real window, check it, keep the evidence

The unit suite paints frames and sends keys from one focus at a time; a bug that
needs the real event loop, real input from every focus, or the retained scene's
partial frames is invisible to it. `tools/qa` drives the shipped binary the way a
person does — on a private Xvfb display, with real pointer and keyboard input —
and checks it with oracles that need no expected screenshot.

## Setup

```bash
sudo apt install xvfb xdotool x11-apps x11-utils python3-pil   # Xvfb, xdotool, xwd, xdpyinfo, Pillow
cmake --build build --target microide -j8
```

Any build works, including the sanitizer builds (`--binary build/microide-asan/microide/microide`).

## Running

```bash
tools/qa/run.py suite                       # every local scenario
tools/qa/run.py suite menus chrome          # some of them
tools/qa/run.py monkey --seed 1 --steps 300 # the seeded monkey
tools/qa/run.py monkey --profile commands --seeds 1-8 --binary build/microide-asan/microide/microide
                                            # the control-command fuzzer (formerly tools/fuzz-control-commands.py)
tools/qa/run.py replay /tmp/mqa-out-.../seed-1/finding-01-.../replay.json
MQA_REMOTE_HOST=worky MQA_REMOTE_ROOT=/home/me/repo tools/qa/run.py remote
tools/run-checks.sh qa                      # the lane: build, suite, 300-step monkey (seed 1)
```

Exit status is non-zero on any finding; the summary names each finding's
directory. `--out DIR` chooses where findings land (default `/tmp/mqa-out-<time>`).

Every run gets its own Xvfb (`-displayfd`, so two drivers can run at once), its own
XDG config/state/data/runtime directories, and a fresh copy of the fixture
(`tools/capture-media/make-fixture.sh`'s `taskflow`), so a seed means the same
actions on the same state and nothing touches your real configuration.

## The oracles

Checked after every action:

- **The redraw verifier.** The app runs with `MICROIDE_QA_VERIFY_REDRAW=1`: after
  every partial frame it renders the same prepared frame in full into a second
  target, diffs the two, and logs `QA-STALE frame=N reason=R pixels=P bbox=x,y,w,h
  first=x,y clips=...` when they differ. That is stale paint — a missed damage rect,
  a wrong surface skip, a frame whose renders read different state — with no
  reference image.
- **Sanitizer and allocator reports** (`ERROR: AddressSanitizer`, `runtime error:`,
  `WARNING: ThreadSanitizer`, glibc's `malloc():` / `double free`), assertions, and
  the process exiting.
- **Scenario expectations**, stated against the `ui` query (`--query ui`: every
  on-screen element by name with its rect, plus focus, modal, sidebar, overlay, menu
  and panel state; see `dev-docs/control/control-channel.md`) — never against pixels.

## Reading a finding directory

```
finding-03-sidebar--click-status-indent/
  findings.txt                 what fired (the QA-STALE line, the report, the expectation)
  trace.txt                    the app's log since the previous action: one
                               `microide redraw frame: frame=N reason=... clips=...` line
                               per frame (MICROIDE_TRACE_REDRAW_VERBOSE), per-clip menu
                               lines, and the QA-STALE lines
  frame-N-retained.png         the verifier's images of stale frame N: what the window
  frame-N-reference.png        showed, and what a full render of the same frame paints
  after-action-NNNN.png        the window after each of the last three actions
  now.png                      the window when the finding was recorded
  replay.json                  the actions up to here; `run.py replay` re-runs them on a
                               fresh fixture
```

Read `findings.txt`, find the frame in `trace.txt` (its reason and clips say which
damage produced it), then diff the two `frame-N-*.png` images — the difference is
the bug. A finding that does not replay still has its frames.

## Turning a finding into a fix

The harness finds; the unit suite pins. Reproduce the finding in-process first —
`tests/support/RetainedRedraw.h` has `DispatchEvents` (events folded as the app loop
folds them) and `PaintAndDiffWithFullRepaint` (retained paint vs a full repaint on
the same canvas) — make it fail, fix, and land the test with the fix. A finding you
do not fix goes into `dev-docs/project/known-tech-debt.md` with its replay file or
scenario name.

## Adding a scenario

A scenario is a function `scenario_x(app)` in `scenarios.py`, registered in
`SCENARIOS`. Drive by name (`app.click_named("menu:Edit")`, `app.hover_named(...)`,
`app.key("ctrl+shift+f")`, `app.send("open", "src/main.cpp")`), call `app.check(label)`
after each action, and state expectations with `app.expect(...)` or
`app.wait_until(...)` against `app.ui()` — never `sleep` for a state the app reports.

## Files

| file | what |
|------|------|
| `microide_qa.py` | `App`: lifecycle, input, control channel, `ui` locators, oracles, evidence, replay |
| `scenarios.py` | the local scenario suite |
| `monkey.py` | the seeded monkey and the command fuzzer's generators, profiles and churn |
| `run.py` | the CLI |
| `remote.py`, `agents.py` | the remote-project scenarios against a real ssh host (on demand) |
