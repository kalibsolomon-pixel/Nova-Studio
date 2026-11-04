import tempfile
import unittest
from pathlib import Path
from generate import generate
from PIL import Image

class FixtureTests(unittest.TestCase):
    def test_reproducible_and_transparent(self):
        with tempfile.TemporaryDirectory() as tmp:
            a, b = Path(tmp)/"a", Path(tmp)/"b"
            self.assertEqual(generate(a, 25), generate(b, 25))
            self.assertEqual(len(list(a.rglob("*.png"))), 25)
            first = Image.open(next(a.rglob("*.png")))
            self.assertEqual(first.mode, "RGBA")
            self.assertEqual(first.getextrema()[3], (0, 255))
            with self.assertRaises(FileExistsError): generate(a, 25)

if __name__ == "__main__": unittest.main()
