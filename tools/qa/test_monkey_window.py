"""--min-logical: a monkey run in a real-size cell keeps a real-size window."""
import random
import unittest

import monkey as m


class ScalingApp:
    """A window as the app keeps it: a change of ui.scale is answered, at the
    app's next idle point, with a resize of its own that keeps the window's size
    in layout units (within the screen)."""

    wayland = False
    size = (1440, 900)

    def __init__(self, pixels, scale):
        self.pixels, self.scale = list(pixels), scale
        self._own_resize = None   # the scale whose resize the app still owes
        self.actions: list[dict] = []
        self.last_ui, self.last_ui_actions = None, -1

    def alive(self):
        return True

    def send_line(self, line):
        self.actions.append({"line": line})
        if line.startswith("set-setting ui.scale "):
            new = float(line.split()[-1])
            units = (self.pixels[0] / self.scale, self.pixels[1] / self.scale)
            self.scale, self._own_resize = new, units

    def resize(self, width, height):
        self.actions.append({"resize": [width, height]})
        self.pixels = [width, height]

    def settle_quiet(self, most=0.0):
        if self._own_resize is not None:
            units, self._own_resize = self._own_resize, None
            self.pixels = [min(self.size[0], round(units[0] * self.scale)),
                           min(self.size[1], round(units[1] * self.scale))]
        return 0.0

    def ui(self):
        return {"window": {"width": round(self.pixels[0] / self.scale), "height": round(self.pixels[1] / self.scale),
                           "scale": [self.scale, self.scale]}}


def monkey_on(app):
    monkey = m.Monkey.__new__(m.Monkey)
    monkey.app, monkey.min_logical, monkey.rng = app, (800, 500), random.Random(5)
    app.last_ui, app.last_ui_actions = app.ui(), len(app.actions)
    return monkey


class KeepWindowUsableTests(unittest.TestCase):
    def test_a_window_left_too_short_is_made_usable_in_one_correction(self):
        # 1440x400 pixels at ui.scale 1: what `ctrl+0` leaves of a window that was
        # 400 pixels high at 0.75 (533 units, allowed).
        app = ScalingApp((1440, 400), 1.0)
        monkey = monkey_on(app)
        monkey._keep_window_usable()
        window = app.ui()["window"]
        self.assertGreaterEqual(window["height"], 500, f"still {window} after {app.actions}")
        self.assertGreaterEqual(window["width"], 800)
        # ... and the next step has nothing to correct: no second scale drawn.
        sent = len(app.actions)
        monkey._keep_window_usable()
        self.assertEqual(len(app.actions), sent, "a usable window is corrected again")

    def test_a_usable_window_is_left_alone(self):
        app = ScalingApp((1440, 900), 1.5)
        monkey = monkey_on(app)
        monkey._keep_window_usable()
        self.assertEqual(app.actions, [])


if __name__ == "__main__":
    unittest.main()
