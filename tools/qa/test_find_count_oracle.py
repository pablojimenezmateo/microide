"""The state oracle's find-widget count: what it compares the widget's count with."""
import unittest

import microide_qa as q


def _findings(text: str, count: int, scope=None, match_case=False) -> list:
    app = q.App.__new__(q.App)
    app.state_evidence = []
    app.query = lambda verb, *a, **k: {"available": True, "kind": "editor", "text": text, "cursors": []}
    elements = [{"name": "button:find/match-case", "active": match_case}]
    overlay = {"visible": True, "mode": "buffer-search", "query": "abcdef", "count": count}
    if scope is not None:
        overlay["scope"] = scope
    return app._state_findings({"elements": elements, "overlay": overlay})


class FindCountOracleTests(unittest.TestCase):
    def test_a_selection_in_a_lossily_decoded_buffer_is_not_counted(self) -> None:
        # Soak ccc6cfdc08e4: binary.bin, Find in Selection over one "abcdef". The
        # reply read each invalid byte as U+FFFD (three bytes), so the selection's
        # byte columns cut the wrong span and the oracle counted none.
        text = "����abcdef�\n"
        self.assertEqual(_findings(text, 1, scope={"start": [1, 5], "end": [1, 11]}), [])

    def test_a_selection_in_valid_text_is_still_counted(self) -> None:
        # The negative control: valid UTF-8, the scope holds one match, the widget says two.
        text = "xx abcdef abcdef\n"
        found = _findings(text, 2, scope={"start": [1, 4], "end": [1, 10]})
        self.assertEqual([f[1] for f in found],
                         ["the find widget counts 2 of 'abcdef'; the buffer holds 1"])

    def test_the_scope_is_cut_before_case_folding(self) -> None:
        # "Ⱥ".lower() is three bytes where "Ⱥ" is two: folding first moved
        # every byte column after it.
        text = "Ⱥ ABCDEF\n"
        self.assertEqual(_findings(text, 1, scope={"start": [1, 4], "end": [1, 10]}), [])

    def test_an_unscoped_count_still_reads_a_lossy_buffer(self) -> None:
        found = _findings("�abcdef�abcdef\n", 1)
        self.assertEqual([f[1] for f in found],
                         ["the find widget counts 1 of 'abcdef'; the buffer holds 2"])


if __name__ == "__main__":
    unittest.main()
