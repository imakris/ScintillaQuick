#!/usr/bin/env python3
"""Keep the audit connected to the dispatch table's actual classifications."""

from collections import Counter
from dataclasses import replace
from pathlib import Path
import unittest

import audit_dispatch_table as audit


class DispatchAuditTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        root = Path(__file__).resolve().parents[2]
        cls.iface = audit.parse_iface(root / "third_party/scintilla/include/Scintilla.iface")
        cls.table = audit.parse_dispatch_table(root / "src/core/scintillaquick_dispatch_table.h")

    def test_setter_classified_like_a_pure_query_fails(self):
        query = next(rule for rule in self.table.rules if rule.macro == "SCI_GETTEXT")
        setter = next(rule for rule in self.table.rules if rule.macro == "SCI_SETTEXT")
        table = replace(self.table, rules=[
            replace(rule, effect=query.effect) if rule == setter else rule
            for rule in self.table.rules
        ])

        result = audit.build_audit(self.iface, table)

        self.assertTrue(any(setter.macro in error for error in result.structural_errors))

    def test_report_counts_the_table_classifications(self):
        result = audit.build_audit(self.iface, self.table)

        for effect, count in Counter(rule.effect for rule in self.table.rules).items():
            with self.subTest(effect=effect):
                self.assertIn(f"    {effect}: {count}", result.report)


if __name__ == "__main__":
    unittest.main()
