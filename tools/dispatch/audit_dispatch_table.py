#!/usr/bin/env python3
"""Audit ScintillaQuick dispatch rules against Scintilla.iface.

This tool is intentionally audit-only. It reads the vendored Scintilla message
inventory and the checked-in ScintillaQuick dispatch rule table, then reports
coverage, drift, and candidate review items. It does not generate production
dispatch output.
"""

from __future__ import annotations

import argparse
from collections import Counter, defaultdict
from dataclasses import dataclass
from pathlib import Path
import re
import sys
from typing import Iterable


IFACE_ENTRY_RE = re.compile(
    r"^(?P<kind>fun|get|set)\s+"
    r"(?P<return_type>\S+)\s+"
    r"(?P<name>[A-Za-z_][A-Za-z0-9_]*)="
    r"(?P<value>\d+)\((?P<params>.*)\)\s*$"
)
TABLE_ENTRY_RE = re.compile(
    r"\{\s*(?P<macro>SCI_[A-Z0-9_]+)\s*,\s*"
    r"Dispatch_effect::(?P<effect>[A-Za-z_]+)\s*,\s*"
    r"(?P<scroll_width_reset>true|false)\s*,\s*"
    r"(?P<known_getter>true|false)\s*\}"
)
CATEGORY_RE = re.compile(r"^cat\s+(?P<category>.+?)\s*$")

QUERY_NAME_PREFIXES = (
    "Get",
    "Count",
    "Point",
    "Position",
    "CharPosition",
    "LineFrom",
    "VisibleFrom",
    "DocLineFrom",
    "WrapCount",
    "BraceMatch",
    "MarkerLine",
    "MarkerHandle",
    "MarkerNumber",
    "MarkerSymbol",
    "AutoCActive",
    "AutoCPosStart",
    "CallTipActive",
    "CallTipPosStart",
    "Supports",
)
STATEFUL_QUERY_LIKE_MACROS = {
    "SCI_SEARCHNEXT",
    "SCI_SEARCHPREV",
    "SCI_SEARCHINTARGET",
    "SCI_REPLACETARGET",
    "SCI_REPLACETARGETRE",
    "SCI_REPLACETARGETMINIMAL",
    "SCI_FORMATRANGE",
    "SCI_FORMATRANGEFULL",
    "SCI_FINDTEXT",
    "SCI_FINDTEXTFULL",
    "SCI_CREATEDOCUMENT",
    "SCI_CREATELOADER",
    "SCI_ALLOCATE",
    "SCI_ALLOCATESUBSTYLES",
    "SCI_PRIVATELEXERCALL",
}
SCROLL_WIDTH_NAME_HINTS = (
    "Text",
    "Style",
    "Font",
    "Tab",
    "Wrap",
    "Margin",
    "Annotation",
    "Representation",
    "Indent",
    "Lexer",
    "Property",
    "CodePage",
    "DocPointer",
)

# Keep this list empty unless ScintillaQuick intentionally keeps a dispatch
# rule for a compatibility macro that no longer appears in Scintilla.iface.
KNOWN_RULES_NOT_IN_IFACE: dict[str, str] = {}


@dataclass(frozen=True)
class IfaceMessage:
    kind: str
    return_type: str
    name: str
    macro: str
    value: int
    params: str
    category: str
    line: int


@dataclass(frozen=True)
class TableRule:
    macro: str
    effect: str
    scroll_width_reset: bool
    known_getter: bool
    line: int


@dataclass
class ParsedIface:
    messages: list[IfaceMessage]
    parse_gaps: list[str]


@dataclass
class ParsedTable:
    rules: list[TableRule]
    parse_gaps: list[str]


@dataclass
class Audit:
    iface: ParsedIface
    table: ParsedTable
    structural_errors: list[str]
    report: str


def macro_name_from_iface_name(name: str) -> str:
    return "SCI_" + name.upper()


def sort_key_by_value(messages_by_macro: dict[str, IfaceMessage], macro: str) -> tuple[int, str]:
    message = messages_by_macro.get(macro)
    if message is None:
        return (sys.maxsize, macro)
    return (message.value, macro)


def parse_iface(path: Path) -> ParsedIface:
    messages: list[IfaceMessage] = []
    parse_gaps: list[str] = []
    category = "Uncategorized"

    try:
        lines = path.read_text(encoding="utf-8").splitlines()
    except OSError as error:
        raise RuntimeError(f"failed to read {path}: {error}") from error

    for number, raw_line in enumerate(lines, start=1):
        line = raw_line.strip()
        if not line or line.startswith("#"):
            continue

        category_match = CATEGORY_RE.match(line)
        if category_match:
            category = category_match.group("category")
            continue

        if not line.startswith(("fun ", "get ", "set ")):
            continue

        match = IFACE_ENTRY_RE.match(line)
        if not match:
            parse_gaps.append(f"{path}:{number}: {raw_line}")
            continue

        name = match.group("name")
        messages.append(
            IfaceMessage(
                kind=match.group("kind"),
                return_type=match.group("return_type"),
                name=name,
                macro=macro_name_from_iface_name(name),
                value=int(match.group("value")),
                params=match.group("params"),
                category=category,
                line=number,
            )
        )

    return ParsedIface(messages=messages, parse_gaps=parse_gaps)


def parse_dispatch_table(path: Path) -> ParsedTable:
    rules: list[TableRule] = []
    parse_gaps: list[str] = []
    in_table = False

    try:
        lines = path.read_text(encoding="utf-8").splitlines()
    except OSError as error:
        raise RuntimeError(f"failed to read {path}: {error}") from error

    for number, raw_line in enumerate(lines, start=1):
        line = raw_line.strip()
        if "k_message_rules[]" in line:
            in_table = True
            continue
        if in_table and line.startswith("};"):
            in_table = False
            continue
        if not in_table:
            continue
        if not line or line.startswith("//") or line.startswith("#"):
            continue

        if "SCI_" not in line:
            continue

        match = TABLE_ENTRY_RE.search(line)
        if not match:
            parse_gaps.append(f"{path}:{number}: {raw_line}")
            continue

        rules.append(
            TableRule(
                macro=match.group("macro"),
                effect=match.group("effect"),
                scroll_width_reset=match.group("scroll_width_reset") == "true",
                known_getter=match.group("known_getter") == "true",
                line=number,
            )
        )

    return ParsedTable(rules=rules, parse_gaps=parse_gaps)


def duplicate_values(items: Iterable[tuple[str, int]]) -> dict[int, list[str]]:
    by_value: dict[int, list[str]] = defaultdict(list)
    for name, value in items:
        by_value[value].append(name)
    return {value: sorted(names) for value, names in by_value.items() if len(names) > 1}


def duplicate_names(names: Iterable[str]) -> dict[str, int]:
    counts = Counter(names)
    return {name: count for name, count in sorted(counts.items()) if count > 1}


def sorted_message_lines(messages: Iterable[IfaceMessage]) -> list[str]:
    return [
        f"  {message.value:4d} {message.macro:<36} {message.kind:<3} "
        f"{message.category} ({message.name})"
        for message in sorted(messages, key=lambda item: (item.value, item.macro))
    ]


def sorted_rule_lines(
    rules: Iterable[TableRule],
    messages_by_macro: dict[str, IfaceMessage],
) -> list[str]:
    return [
        f"  {sort_key_by_value(messages_by_macro, rule.macro)[0]:4d} {rule.macro:<36} "
        f"{rule.effect:<24} scroll_width_reset={str(rule.scroll_width_reset).lower():<5} "
        f"known_getter={str(rule.known_getter).lower():<5}"
        for rule in sorted(rules, key=lambda item: sort_key_by_value(messages_by_macro, item.macro))
    ]


def lines_or_none(lines: list[str], limit: int | None = None) -> list[str]:
    if not lines:
        return ["  none"]
    if limit is not None and len(lines) > limit:
        shown = lines[:limit]
        return shown + [f"  ... {len(lines) - limit} more"]
    return lines


def is_query_looking_fun(message: IfaceMessage) -> bool:
    if message.kind != "fun":
        return False
    if message.name.startswith("Can") and len(message.name) > 3 and message.name[3].isupper():
        return True
    if message.name.startswith("Find"):
        return message.return_type != "void" or "stringresult" in message.params
    if message.name.startswith(QUERY_NAME_PREFIXES):
        return True
    return message.return_type != "void" and "stringresult" in message.params


def is_possible_scroll_width_candidate(message: IfaceMessage) -> bool:
    if message.kind == "get":
        return False
    return any(hint in message.name for hint in SCROLL_WIDTH_NAME_HINTS)


def build_audit(iface: ParsedIface, table: ParsedTable) -> Audit:
    structural_errors: list[str] = []

    if not iface.messages:
        structural_errors.append("Scintilla.iface parse produced no fun/get/set messages")
    if not table.rules:
        structural_errors.append("dispatch table parse produced no k_message_rules entries")

    messages_by_macro = {message.macro: message for message in iface.messages}
    rules_by_macro = {rule.macro: rule for rule in table.rules}

    iface_name_duplicates = duplicate_names(message.macro for message in iface.messages)
    iface_id_duplicates = duplicate_values((message.macro, message.value) for message in iface.messages)
    table_name_duplicates = duplicate_names(rule.macro for rule in table.rules)
    table_id_duplicates = duplicate_values(
        (rule.macro, messages_by_macro[rule.macro].value)
        for rule in table.rules
        if rule.macro in messages_by_macro
    )

    if iface.parse_gaps:
        structural_errors.append(f"failed to parse {len(iface.parse_gaps)} iface message lines")
    if table.parse_gaps:
        structural_errors.append(f"failed to parse {len(table.parse_gaps)} dispatch table lines")
    if table_name_duplicates:
        structural_errors.append(
            "duplicate dispatch table entries: "
            + ", ".join(f"{name} x{count}" for name, count in table_name_duplicates.items())
        )
    if table_id_duplicates:
        structural_errors.append(
            "duplicate dispatch table ids: "
            + ", ".join(
                f"{value} ({', '.join(names)})"
                for value, names in sorted(table_id_duplicates.items())
            )
        )

    missing_from_iface = sorted(
        [rule.macro for rule in table.rules if rule.macro not in messages_by_macro],
        key=lambda macro: (macro not in KNOWN_RULES_NOT_IN_IFACE, macro),
    )
    unexpected_missing = [
        macro for macro in missing_from_iface if macro not in KNOWN_RULES_NOT_IN_IFACE
    ]
    if unexpected_missing:
        structural_errors.append(
            "dispatch rules missing from Scintilla.iface without an explicit explanation: "
            + ", ".join(unexpected_missing)
        )

    set_read_only = [
        messages_by_macro[rule.macro]
        for rule in table.rules
        if rule.macro in messages_by_macro
        and messages_by_macro[rule.macro].kind == "set"
        and rule.effect == "Read_only"
    ]
    if set_read_only:
        structural_errors.append(
            "set messages classified Read_only: "
            + ", ".join(message.macro for message in set_read_only)
        )

    table_order_keys = [
        sort_key_by_value(messages_by_macro, rule.macro) for rule in table.rules
    ]
    if table_order_keys != sorted(table_order_keys):
        structural_errors.append("dispatch table entries are not ordered deterministically by id/name")

    by_kind = Counter(message.kind for message in iface.messages)
    by_category = Counter(message.category for message in iface.messages)
    deprecated_messages = [
        message for message in iface.messages if message.category == "Deprecated"
    ]
    provisional_messages = [
        message for message in iface.messages if message.category == "Provisional"
    ]
    values = [message.value for message in iface.messages]

    effect_counts = Counter(rule.effect for rule in table.rules)
    unclassified_by_kind: dict[str, list[IfaceMessage]] = {
        kind: sorted(
            [
                message
                for message in iface.messages
                if message.kind == kind and message.macro not in rules_by_macro
            ],
            key=lambda item: (item.value, item.macro),
        )
        for kind in ("get", "set", "fun")
    }

    get_not_read_only = [
        message
        for message in iface.messages
        if message.kind == "get"
        and (
            message.macro not in rules_by_macro
            or rules_by_macro[message.macro].effect != "Read_only"
        )
    ]
    read_only_fun = [
        message
        for message in iface.messages
        if message.kind == "fun"
        and message.macro in rules_by_macro
        and rules_by_macro[message.macro].effect == "Read_only"
    ]
    query_fun_absent = [
        message
        for message in iface.messages
        if is_query_looking_fun(message) and message.macro not in rules_by_macro
    ]
    stateful_unclassified = [
        messages_by_macro[macro]
        for macro in sorted(
            STATEFUL_QUERY_LIKE_MACROS & messages_by_macro.keys(),
            key=lambda item: (messages_by_macro[item].value, item),
        )
        if macro not in rules_by_macro
    ]
    scroll_width_candidates = [
        message
        for message in iface.messages
        if is_possible_scroll_width_candidate(message)
        and (
            message.macro not in rules_by_macro
            or not rules_by_macro[message.macro].scroll_width_reset
        )
    ]

    report_lines: list[str] = []
    report_lines.extend(
        [
            "ScintillaQuick Dispatch Table Audit",
            "====================================",
            "",
            "Inputs:",
            "  third_party/scintilla/include/Scintilla.iface",
            "  src/core/scintillaquick_dispatch_table.h",
            "",
            "Message Inventory",
            "-----------------",
            f"  total iface messages: {len(iface.messages)}",
            f"  min id: {min(values) if values else 'n/a'}",
            f"  max id: {max(values) if values else 'n/a'}",
            f"  deprecated messages: {len(deprecated_messages)}",
            f"  provisional messages: {len(provisional_messages)}",
            "  by kind:",
        ]
    )
    for kind in ("get", "set", "fun"):
        report_lines.append(f"    {kind}: {by_kind.get(kind, 0)}")
    report_lines.append("  by category:")
    for category, count in sorted(by_category.items(), key=lambda item: item[0]):
        report_lines.append(f"    {category}: {count}")
    report_lines.append("")

    report_lines.extend(
        [
            "Manual-Table Coverage",
            "---------------------",
            f"  rule count: {len(table.rules)}",
            f"  known_getter count: {sum(1 for rule in table.rules if rule.known_getter)}",
            f"  scroll_width_reset count: {sum(1 for rule in table.rules if rule.scroll_width_reset)}",
            "  by effect:",
        ]
    )
    for effect in ("Read_only", "Overlay", "Static_content", "Static_content_and_style", "Scroll"):
        report_lines.append(f"    {effect}: {effect_counts.get(effect, 0)}")
    report_lines.append("")

    report_lines.extend(["Unclassified Messages", "---------------------"])
    for kind in ("get", "set", "fun"):
        messages = unclassified_by_kind[kind]
        report_lines.append(f"  {kind}: {len(messages)}")
        report_lines.extend(lines_or_none(sorted_message_lines(messages), limit=80))
    report_lines.append("")

    report_lines.extend(["Drift / Duplicates / Parse Gaps", "-------------------------------"])
    report_lines.append("  table rules missing from iface:")
    if missing_from_iface:
        for macro in missing_from_iface:
            reason = KNOWN_RULES_NOT_IN_IFACE.get(macro, "missing explicit explanation")
            report_lines.append(f"    {macro}: {reason}")
    else:
        report_lines.append("    none")
    report_lines.append("  iface duplicate names:")
    report_lines.extend(
        lines_or_none([f"    {name}: {count}" for name, count in iface_name_duplicates.items()])
    )
    report_lines.append("  iface duplicate ids:")
    report_lines.extend(
        lines_or_none(
            [
                f"    {value}: {', '.join(names)}"
                for value, names in sorted(iface_id_duplicates.items())
            ]
        )
    )
    report_lines.append("  table duplicate names:")
    report_lines.extend(
        lines_or_none([f"    {name}: {count}" for name, count in table_name_duplicates.items()])
    )
    report_lines.append("  table duplicate ids:")
    report_lines.extend(
        lines_or_none(
            [
                f"    {value}: {', '.join(names)}"
                for value, names in sorted(table_id_duplicates.items())
            ]
        )
    )
    report_lines.append("  iface parse gaps:")
    report_lines.extend(lines_or_none([f"    {gap}" for gap in iface.parse_gaps], limit=40))
    report_lines.append("  table parse gaps:")
    report_lines.extend(lines_or_none([f"    {gap}" for gap in table.parse_gaps], limit=40))
    report_lines.append("")

    report_lines.extend(["Getter / Mutator Ambiguity", "---------------------------"])
    report_lines.append(f"  get entries not classified Read_only: {len(get_not_read_only)}")
    report_lines.extend(lines_or_none(sorted_message_lines(get_not_read_only), limit=80))
    report_lines.append(f"  set entries classified Read_only: {len(set_read_only)}")
    report_lines.extend(lines_or_none(sorted_message_lines(set_read_only), limit=80))
    report_lines.append(f"  fun entries classified Read_only: {len(read_only_fun)}")
    report_lines.extend(lines_or_none(sorted_message_lines(read_only_fun), limit=120))
    report_lines.append("")

    report_lines.extend(["Override Candidates", "-------------------"])
    report_lines.append(f"  query-looking fun messages absent from the rule table: {len(query_fun_absent)}")
    report_lines.extend(lines_or_none(sorted_message_lines(query_fun_absent), limit=120))
    report_lines.append(f"  stateful query-looking messages intentionally conservative: {len(stateful_unclassified)}")
    report_lines.extend(lines_or_none(sorted_message_lines(stateful_unclassified), limit=80))
    report_lines.append(f"  possible scroll-width reset candidates: {len(scroll_width_candidates)}")
    report_lines.extend(lines_or_none(sorted_message_lines(scroll_width_candidates), limit=120))
    report_lines.append("")

    report_lines.extend(["Structural Check", "----------------"])
    if structural_errors:
        report_lines.append("  FAIL")
        report_lines.extend(f"  - {error}" for error in structural_errors)
    else:
        report_lines.append("  PASS")
    report_lines.append("")

    return Audit(
        iface=iface,
        table=table,
        structural_errors=structural_errors,
        report="\n".join(report_lines),
    )


def run(repo_root: Path, check: bool) -> int:
    iface_path = repo_root / "third_party" / "scintilla" / "include" / "Scintilla.iface"
    table_path = repo_root / "src" / "core" / "scintillaquick_dispatch_table.h"

    try:
        iface = parse_iface(iface_path)
        table = parse_dispatch_table(table_path)
        audit = build_audit(iface, table)
    except RuntimeError as error:
        print(f"error: {error}", file=sys.stderr)
        return 1

    print(audit.report)
    if check and audit.structural_errors:
        return 1
    return 0


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(
        description="Audit ScintillaQuick dispatch rules against Scintilla.iface."
    )
    parser.add_argument(
        "--repo-root",
        type=Path,
        default=Path(__file__).resolve().parents[2],
        help="repository root; defaults to two directories above this script",
    )
    parser.add_argument(
        "--check",
        action="store_true",
        help="exit non-zero for structural audit failures",
    )
    args = parser.parse_args(argv)

    repo_root = args.repo_root.resolve()
    return run(repo_root, args.check)


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
