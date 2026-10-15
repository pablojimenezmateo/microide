## ADDED Requirements

### Requirement: Partial frames are verified against a full render on demand

When the environment variable `MICROIDE_QA_VERIFY_REDRAW` is `1` at startup, the application SHALL, after presenting every frame it rendered partially (from dirty rects, not a full redraw), render the same prepared frame in full into a separate offscreen target, compare the two images pixel by pixel, and report any difference. The comparison SHALL use the frame exactly as the user saw it -- the retained scene after the partial render -- not a re-render of it.

#### Scenario: A correct partial frame reports nothing
- **WHEN** the verifier is on and a partial frame repaints the rects its damage asked for and every other pixel was already current
- **THEN** no report is written for that frame

#### Scenario: A stale partial frame is reported
- **WHEN** the verifier is on and a partial frame leaves pixels that differ from a full render of the same state (for example a surface skipped while the frame base painted over it)
- **THEN** one log line beginning `QA-STALE` is written for that frame, carrying the bounding box and count of differing pixels, the frame's merged clip rects, and the frame's redraw reason

#### Scenario: Full frames are not compared
- **WHEN** the verifier is on and a frame is rendered in full (requested full, promoted to full, or a fallback)
- **THEN** no comparison is made for it, because there is nothing retained to be stale

### Requirement: The verifier costs nothing when off

With `MICROIDE_QA_VERIFY_REDRAW` unset or not `1`, the application SHALL allocate no second render target, read back no pixels and perform no comparison; the only cost SHALL be one flag check per frame.

#### Scenario: Normal runs are unaffected
- **WHEN** the application runs without the variable
- **THEN** frame rendering does the same work as before the verifier existed, and the perf gate is unchanged

### Requirement: Verifier findings do not change what the user sees

The verifier SHALL NOT repair a stale frame: the retained scene and the presented frame SHALL be exactly what the partial render produced, so a finding can also be seen and screenshotted.

#### Scenario: The stale frame stays on screen
- **WHEN** the verifier reports a stale frame
- **THEN** the presented window still shows the stale pixels until the application's own next repaint covers them
