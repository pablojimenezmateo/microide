"""What the soak keeps on disk: corpus prefixes it can use, and a fixed
finding's record without the evidence the fix made moot."""
import json
import os
import random
import sys
import tempfile
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parents[0] / "qa"))
import monkey_model as mm  # noqa: E402

HEADER = {"fixture": "readme", "size": [640, 480]}


def actions(n: int) -> list[dict]:
    return [{"key": "a"}] * n


class CorpusTests(unittest.TestCase):
    def setUp(self):
        self.dir = Path(tempfile.mkdtemp(dir=os.environ.get("TMPDIR")))
        self.corpus = mm.Corpus(self.dir)

    def test_a_prefix_too_long_to_pick_is_not_saved(self):
        self.assertTrue(self.corpus.save("short", HEADER, actions(mm.MAX_PREFIX_ACTIONS)))
        self.assertFalse(self.corpus.save("long", HEADER, actions(mm.MAX_PREFIX_ACTIONS + 1)))
        self.assertEqual(len(list(self.dir.glob("*.json"))), 1)

    def test_every_saved_prefix_can_be_picked(self):
        self.corpus.save("s", HEADER, actions(mm.MAX_PREFIX_ACTIONS))
        record = self.corpus.pick(random.Random(1), mm.Coverage(), HEADER)
        self.assertEqual(record["state"], "s")

    def test_prune_drops_the_prefixes_pick_skips(self):
        self.corpus.save("keep", HEADER, actions(3))
        stale = self.dir / "0123456789abcdef.json"
        stale.write_text(json.dumps({**HEADER, "state": "old", "actions": actions(mm.MAX_PREFIX_ACTIONS + 1)}))
        (self.dir / "broken0000000000.json").write_text("{")
        removed, freed = self.corpus.prune()
        self.assertEqual(removed, 2)
        self.assertGreater(freed, 0)
        self.assertEqual([json.loads(p.read_text())["state"] for p in self.dir.glob("*.json")], ["keep"])


class VerifiedBundleTests(unittest.TestCase):
    def test_a_verified_bundle_keeps_its_record_and_drops_its_evidence(self):
        os.environ["MICROIDE_SOAK_DIR"] = tempfile.mkdtemp(dir=os.environ.get("TMPDIR"))
        sys.path.insert(0, str(HERE))
        import importlib
        import soak
        importlib.reload(soak)
        bundle = soak.FINDINGS / "abc123abc123"
        for name in ("evidence/frame.png", "sightings/s1/frame.png", "verify/run0/log.txt"):
            (bundle / name).parent.mkdir(parents=True, exist_ok=True)
            (bundle / name).write_bytes(b"x" * 1000)
        for name in ("FINDING.md", "meta.json", "minimal.json", "replay.json"):
            (bundle / name).write_text("{}")
        freed = soak.prune_verified_bundle(bundle)
        self.assertEqual(freed, 3000)
        self.assertEqual(sorted(p.name for p in bundle.iterdir()),
                         ["FINDING.md", "meta.json", "minimal.json", "replay.json"])


if __name__ == "__main__":
    unittest.main()
