#!/usr/bin/env python3
from __future__ import annotations

import argparse
import ast
import re
import subprocess
import sys
from pathlib import Path


PRINTABLE_ASCII = "".join(chr(code) for code in range(0x20, 0x7F))
STRING_LITERAL_RE = re.compile(r'"(?:\\.|[^"\\])*"')


def parse_args() -> argparse.Namespace:
    fonts_dir = Path(__file__).resolve().parent
    repo_root = fonts_dir.parent.parent
    default_charset = fonts_dir / "ui_font_charset.txt"
    default_extra = fonts_dir / "ui_font_extra_chars.txt"

    parser = argparse.ArgumentParser(description="Generate a Tiny TTF subset for the demo UI.")
    parser.add_argument("--source", required=True, help="Path to the original TTF file.")
    parser.add_argument("--output", default=str(fonts_dir / "NotoSansSC-Regular.ttf"),
                        help="Output subset TTF path.")
    parser.add_argument("--charset-output", default=str(default_charset),
                        help="Path used to write the merged charset text.")
    parser.add_argument("--extra-chars", default=str(default_extra),
                        help="Path to additional characters kept for dynamic text such as SSID.")
    parser.add_argument("--scan-root", default=str(repo_root / "main"),
                        help="Directory containing UI source files to scan.")
    return parser.parse_args()


def extract_chars_from_c_sources(scan_root: Path) -> set[str]:
    chars: set[str] = set()

    for path in sorted(scan_root.rglob("*")):
        if path.suffix not in {".c", ".h"}:
            continue

        text = path.read_text(encoding="utf-8")
        for token in STRING_LITERAL_RE.findall(text):
            try:
                literal = ast.literal_eval(token)
            except (SyntaxError, ValueError):
                continue

            for ch in literal:
                if ch.isprintable():
                    chars.add(ch)

    return chars


def read_extra_chars(path: Path) -> set[str]:
    chars: set[str] = set()

    for raw_line in path.read_text(encoding="utf-8").splitlines():
        line = raw_line.strip()
        if not line or line.startswith("#"):
            continue

        for ch in line:
            if not ch.isspace():
                chars.add(ch)

    return chars


def write_charset(path: Path, chars: set[str]) -> None:
    ordered = "".join(sorted(chars))
    path.write_text(ordered + "\n", encoding="utf-8")


def run_subset(source: Path, output: Path, charset_path: Path) -> None:
    cmd = [
        sys.executable,
        "-m",
        "fontTools.subset",
        str(source),
        f"--output-file={output}",
        f"--text-file={charset_path}",
        "--layout-features=*",
        "--glyph-names",
        "--symbol-cmap",
        "--legacy-cmap",
        "--notdef-glyph",
        "--notdef-outline",
        "--recommended-glyphs",
        "--name-IDs=*",
        "--name-legacy",
        "--name-languages=*",
        "--no-hinting",
    ]
    subprocess.run(cmd, check=True)


def main() -> int:
    args = parse_args()

    source = Path(args.source).resolve()
    output = Path(args.output).resolve()
    charset_output = Path(args.charset_output).resolve()
    extra_chars = Path(args.extra_chars).resolve()
    scan_root = Path(args.scan_root).resolve()

    if not source.is_file():
        raise FileNotFoundError(f"source font not found: {source}")
    if not extra_chars.is_file():
        raise FileNotFoundError(f"extra chars file not found: {extra_chars}")
    if not scan_root.is_dir():
        raise NotADirectoryError(f"scan root not found: {scan_root}")

    charset = set(PRINTABLE_ASCII)
    charset.update(extract_chars_from_c_sources(scan_root))
    charset.update(read_extra_chars(extra_chars))

    output.parent.mkdir(parents=True, exist_ok=True)
    charset_output.parent.mkdir(parents=True, exist_ok=True)
    write_charset(charset_output, charset)
    run_subset(source, output, charset_output)

    print(f"charset_count={len(charset)}")
    print(f"charset_file={charset_output}")
    print(f"subset_font={output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
