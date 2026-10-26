"""Evidence must preserve identifiers while exposing counters as numbers."""
from verification import require  # also refuses optimized execution before fixture imports
import unittest
from monitor_integration import parse


class DiagnosticTests(unittest.TestCase):
    def test_sqlstate_is_an_identifier(self):
        for code in ("08001", "08006", "42501", "55P03", "none"):
            with self.subTest(code=code):
                row = parse(f"persistence connected=0 last_error={code} query_failures=12")
                self.assertEqual(row["last_error"], code)
                self.assertEqual(row["connected"], 0)
                self.assertEqual(row["query_failures"], 12)


if __name__ == "__main__":
    unittest.main()
