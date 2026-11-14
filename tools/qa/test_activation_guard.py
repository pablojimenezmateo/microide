#!/usr/bin/env python3
"""A key the monkey sends never chooses Quit from an open menu
(App.activation_guard, Monkey._activation_chooses_forbidden): arrow keys walked
a menu's selection onto Exit, a typed newline chose it, and the app's clean
exit was filed as a crash (soak 7ab9a5476905)."""
from __future__ import annotations

import sys
import unittest
from pathlib import Path
from unittest.mock import Mock

sys.path.insert(0, str(Path(__file__).resolve().parent))

import microide_qa as q  # noqa: E402
import monkey as mk  # noqa: E402


class _StubApp(q.App):
    wayland = False  # the X path; its input is stubbed below


def _app(guard_answers: list[bool]) -> q.App:
    """An App with its X input stubbed: what it sends is what it records."""
    app = _StubApp.__new__(_StubApp)
    app.recorded = []
    app._record = app.recorded.append
    app._focus = Mock()
    app._expect_typed_input = Mock()
    app._x_keys = Mock()
    app._x_type_ascii = Mock()
    answers = iter(guard_answers)
    app.activation_guard = lambda: next(answers, False)
    return app


class ActivationGuardTests(unittest.TestCase):
    def test_return_on_a_forbidden_row_closes_the_menu_first(self) -> None:
        app = _app([True])
        app.key("Return")
        self.assertEqual(app.recorded, [{"key": "Escape"}, {"key": "Return"}])

    def test_a_typed_newline_is_guarded_too(self) -> None:
        app = _app([True])
        app.type("\n\n\n")
        self.assertEqual(app.recorded[0], {"key": "Escape"})

    def test_keys_that_only_move_are_not_asked_about(self) -> None:
        app = _app([True])
        app.key("alt+Up")
        self.assertEqual(app.recorded, [{"key": "alt+Up"}])

    def test_a_harmless_selection_is_left_alone(self) -> None:
        app = _app([False])
        app.key("Return")
        self.assertEqual(app.recorded, [{"key": "Return"}])


class MonkeyGuardTests(unittest.TestCase):
    def _monkey(self, ui: dict) -> mk.Monkey:
        monkey = mk.Monkey.__new__(mk.Monkey)
        monkey.app = Mock()
        monkey.app.ui.return_value = ui
        return monkey

    def test_quit_selected_in_an_open_menu_is_forbidden(self) -> None:
        ui = {"menu": {"open": True}, "elements": [
            {"kind": "menu-item", "name": "menu/File/Save", "label": "Save", "active": False},
            {"kind": "menu-item", "name": "menu/File/Quit", "label": "Quit", "active": True}]}
        self.assertTrue(self._monkey(ui)._activation_chooses_forbidden())

    def test_another_row_selected_is_not(self) -> None:
        ui = {"menu": {"open": True}, "elements": [
            {"kind": "menu-item", "name": "menu/File/Save", "label": "Save", "active": True},
            {"kind": "menu-item", "name": "menu/File/Quit", "label": "Quit", "active": False}]}
        self.assertFalse(self._monkey(ui)._activation_chooses_forbidden())

    def test_no_menu_open_is_not(self) -> None:
        ui = {"menu": {"open": False}, "elements": [
            {"kind": "menu-item", "name": "menu/File/Quit", "label": "Quit", "active": True}]}
        self.assertFalse(self._monkey(ui)._activation_chooses_forbidden())


class PressGuardTests(unittest.TestCase):
    def test_a_press_on_the_edge_of_quit_is_refused(self) -> None:
        # Soak 7ab9a5476905 (again, at ui.scale 1.25): the File menu's Quit row was
        # [14, 315, 312, 22], the press (326, 336) -- its right edge, outside by the
        # rect, but the point is rounded to device pixels on the way in and the app
        # took it at 325: Quit, and the app's clean exit filed as a crash.
        monkey = mk.Monkey.__new__(mk.Monkey)
        monkey.app = Mock()
        monkey.app.ui.return_value = {"elements": [
            {"kind": "menu-item", "name": "menu-item:File/Quit", "label": "Quit", "rect": [14, 315, 312, 22]}]}
        self.assertTrue(monkey._press_lands_on_forbidden(326, 336))
        self.assertTrue(monkey._press_lands_on_forbidden(14 - 1, 320))
        self.assertFalse(monkey._press_lands_on_forbidden(400, 336), "well away from it is fine")


if __name__ == "__main__":
    unittest.main()
