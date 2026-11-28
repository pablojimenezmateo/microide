"""The `textInput` oracle (microide_qa._text_input_findings, TD-2026-11-08-435
item 2): typed text and the keys go to the same surface. Typing follows the
`ui` answer's `textInput`, Enter and Backspace its `focus`; when they part, a
caret blinks where the keys do not go."""
import unittest

import microide_qa as q


def ui(focus: str, text_input: str, modal: str | None = None) -> dict:
    return {"focus": focus, "textInput": text_input, "modal": modal}


class TextInputOracleTests(unittest.TestCase):
    def test_typing_where_the_focus_is_is_fine(self):
        for focus, surface in (("editor", "editor"), ("overlay", "command-palette"), ("overlay", "buffer-search"),
                               ("sidebar", "sidebar-search-query"), ("sidebar", "commit-body"),
                               ("panel", "terminal"), ("panel", "terminal-find"),
                               ("debug-pane", "debug-variable-edit")):
            self.assertEqual(q._text_input_findings(ui(focus, surface)), [], f"{surface} under {focus}")

    def test_typing_that_goes_elsewhere_is_a_finding(self):
        found = q._text_input_findings(ui("editor", "sidebar-search-query"))
        self.assertEqual(len(found), 1)
        self.assertEqual(found[0][0], "focus")
        self.assertIn("sidebar-search-query", found[0][1])
        self.assertIn("editor", found[0][1])
        self.assertTrue(q._text_input_findings(ui("sidebar", "terminal")))

    def test_a_completion_list_keeps_the_typing_in_the_editor(self):
        listing = dict(ui("overlay", "editor"), overlay={"visible": True, "mode": "completion"})
        self.assertEqual(q._text_input_findings(listing), [], "the arrows go to the list, the letters to the buffer")
        palette = dict(ui("overlay", "editor"), overlay={"visible": True, "mode": "command-palette"})
        self.assertTrue(q._text_input_findings(palette), "under the palette the editor takes nothing")

    def test_what_it_cannot_judge_it_leaves(self):
        self.assertEqual(q._text_input_findings(ui("editor", "none")), [], "nothing takes text")
        self.assertEqual(q._text_input_findings(ui("editor", "prompt-input", modal="prompt")), [],
                         "a modal owns the keys")
        self.assertEqual(q._text_input_findings(ui("editor", "a-surface-added-later")), [], "unknown")
        self.assertEqual(q._text_input_findings({"focus": "editor"}), [], "an app that does not say")


if __name__ == "__main__":
    unittest.main()
