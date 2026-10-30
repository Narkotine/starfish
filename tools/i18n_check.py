#!/usr/bin/env python3
"""Keep the French catalogue complete, and keep it honest.

F-036 promises an interface "at least in English and French". A catalogue that
misses one string does not fail loudly: the interface simply shows that string in
the source language, inside an otherwise French window -- and the only person who
notices is the one using it. So the two halves are checked here, in CI:

  * every translatable string the code carries has an entry in
    `src/i18n/starfish_fr.ts`, with a non-empty, *finished* translation;
  * the catalogue holds nothing the code no longer has (a stale entry means
    lupdate was never re-run, and the file drifts away from the code).

The compiled `.qm` is deliberately **not** built here: `lrelease` needs a Qt
LinguistTools installation, and a build machine without one would then fail over
a translation. What the compiled catalogue really does is checked where it
matters -- `--product-probe`, which asks the product's own window what language it
is speaking.

Translatable strings are found by scanning the sources, which is what `lupdate`
does too; the scan is deliberately simple (a stripped of comments tree, literal
strings only) and it says so when it cannot read a call. Run `lupdate` from the Qt
installation to regenerate the catalogue:

    lupdate -no-obsolete -locations relative src -ts src/i18n/starfish_fr.ts
"""

from __future__ import annotations

import re
import sys
import xml.etree.ElementTree as ElementTree
from pathlib import Path

CATALOG = "src/i18n/starfish_fr.ts"
SOURCE_DIRS = ("src",)

# `tr("...")` (any class), `tr(QLatin1String("..."))`, and the explicit form a
# static function uses: `QCoreApplication::translate("Context", "...")`.
#
# A context that starts with "Q" is Qt's own ("QPlatformTheme", "QColorDialog"):
# reading one of those is a *lookup* of the toolkit's translation (that is how the
# probe checks it), not a string StarFish has to translate.
# Adjacent literals ("a " "b") are one string to the compiler and to lupdate,
# so they are one here too; QT_TR_NOOP marks a string tr() reads later (a table).
LITERALS = r'"(?:[^"\\]|\\.)*"(?:\s*"(?:[^"\\]|\\.)*")*'
CALL_PATTERNS = [
    re.compile(r'\btr\(\s*(?P<text>' + LITERALS + ')'),
    re.compile(r'\bQT_TR_NOOP\(\s*(?P<text>' + LITERALS + ')'),
    re.compile(r'\btr\(\s*QLatin1String\(\s*(?P<text>' + LITERALS + ')'),
    re.compile(r'\btranslate\(\s*"(?P<context>[^"]*)"\s*,\s*(?P<text>' + LITERALS + ')'),
]
PIECE = re.compile(r'"((?:[^"\\]|\\.)*)"')


def joined(literals: str) -> str:
    return "".join(PIECE.findall(literals))

BLOCK_COMMENT = re.compile(r"/\*.*?\*/", re.S)
LINE_COMMENT = re.compile(r"//[^\n]*")
# A call whose argument does not close on the same line: reported, never guessed.
SPANNING_CALL = re.compile(r"\btr\(\s*$", re.M)


def canonical(text: str) -> str:
    """The same string, whichever side of the catalogue it comes from.

    Both sides escape: C++ writes `\\` for one backslash and `\\"` for a quote,
    and the TS file carries the same escaping (plus XML's). Comparing the
    *unescaped* value on both sides is what makes the comparison meaningful."""
    return text.replace('\\"', '"').replace("\\\\", "\\")


def translatable_strings(root: Path) -> tuple[set[str], list[str]]:
    found: set[str] = set()
    unreadable: list[str] = []
    for directory in SOURCE_DIRS:
        for path in sorted((root / directory).rglob("*")):
            if path.suffix not in {".cpp", ".hpp"}:
                continue
            body = path.read_text(encoding="utf-8", errors="replace")
            body = LINE_COMMENT.sub("", BLOCK_COMMENT.sub("", body))
            for match in SPANNING_CALL.finditer(body):
                # `tr(` at the end of a line: the argument continues on the next,
                # so a literal reader cannot say what the string is. Say so rather
                # than reporting the first line as a missing translation.
                line = body[: match.start()].count("\n") + 1
                unreadable.append(f"{path.relative_to(root)}:{line}")
            for pattern in CALL_PATTERNS:
                for match in pattern.finditer(body):
                    context = match.groupdict().get("context")
                    if context and context.startswith("Q"):
                        continue  # Qt's own string, looked up and not ours
                    found.add(canonical(joined(match.group("text"))))
    return found, unreadable


def catalogue_entries(root: Path) -> dict[str, str | None]:
    """source -> translation (None when it is unfinished or empty)."""
    tree = ElementTree.parse(root / CATALOG)
    entries: dict[str, str | None] = {}
    for message in tree.getroot().iter("message"):
        source = message.findtext("source") or ""
        translation = message.find("translation")
        if translation is None:
            entries[canonical(source)] = None
            continue
        text = (translation.text or "").strip()
        unfinished = translation.get("type") == "unfinished"
        entries[canonical(source)] = None if (unfinished or not text) else text
    return entries


def main() -> int:
    root = Path(__file__).resolve().parent.parent
    if not (root / CATALOG).is_file():
        print(f"MISSING  {CATALOG}")
        return 1

    wanted, unreadable = translatable_strings(root)
    entries = catalogue_entries(root)

    missing = sorted(text for text in wanted if text not in entries)
    untranslated = sorted(text for text, value in entries.items() if value is None)
    stale = sorted(text for text in entries if text not in wanted)

    for path in unreadable:
        print(f"note     a tr() call spans lines, not read here: {path}")

    print(f"ok       {len(wanted)} translatable string(s) in the code, "
          f"{len(entries)} entr(ies) in {CATALOG}")

    failures = False
    for label, texts, hint in (
        ("MISSING", missing, "run lupdate, then translate the new entries"),
        ("UNTRANSLATED", untranslated, f"fill them in {CATALOG}"),
        ("STALE", stale, "run lupdate -no-obsolete, then check the diff"),
    ):
        if not texts:
            continue
        failures = True
        print(f"{label}  {len(texts)} string(s) -- {hint}:")
        for text in texts:
            print(f"           {text}")

    if failures:
        print("\nThe catalogue is what the French interface speaks: fix it, "
              "do not drop the check.")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
