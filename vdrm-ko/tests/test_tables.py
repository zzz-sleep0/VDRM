import importlib.util
from pathlib import Path
import tempfile
import unittest

spec = importlib.util.spec_from_file_location('port', Path(__file__).resolve().parents[1] / 'scripts/port.py')
port = importlib.util.module_from_spec(spec)
spec.loader.exec_module(port)

class TableTests(unittest.TestCase):
    def parse(self, text):
        with tempfile.TemporaryDirectory() as d:
            p = Path(d) / 'table.tsv'
            p.write_text(text)
            return port.table(p)

    def test_valid(self):
        self.assertEqual(self.parse('fget\t0x314a6896\n'), {'fget': 0x314a6896})

    def test_truncated(self):
        with self.assertRaises(ValueError):
            self.parse('fget 0x314a6896')

    def test_duplicate(self):
        with self.assertRaises(ValueError):
            self.parse('fget 0x314a6896\nfget 0x314a6896\n')

    def test_conflicting_duplicate(self):
        with self.assertRaises(ValueError):
            self.parse('fget 0x314a6896\nfget 0x314a6897\n')

    def test_bad_value(self):
        with self.assertRaises(ValueError):
            self.parse('fget 0x123\n')

    def test_injection(self):
        with self.assertRaises(ValueError):
            self.parse('bad"name 0x314a6896\n')

    def test_unsupported_config(self):
        with self.assertRaises(ValueError):
            port.flags({'CONFIG_MODULE_REL_CRCS=y'})

if __name__ == '__main__':
    unittest.main()
