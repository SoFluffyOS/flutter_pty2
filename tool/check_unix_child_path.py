#!/usr/bin/env python3
"""Guard the Unix post-fork child branch against non-async-safe helpers."""

from pathlib import Path
import re
import sys


CHILD_START = "    if (child == 0) {"
CHILD_END = "    }\n\n    close(status_fd);"
FORBIDDEN_PATTERNS = (
    r"\b(?:malloc|calloc|realloc|free|getenv|snprintf|printf)\s*\(",
    r"\bstr[A-Za-z0-9_]*\s*\(",
    r"\bPATH\b",
    r"\bpthread_[A-Za-z0-9_]*",
    r"\bDart_[A-Za-z0-9_]*",
    r"\bexecvp\s*\(",
)


def main() -> int:
    if len(sys.argv) != 2:
        print(f"usage: {sys.argv[0]} SOURCE", file=sys.stderr)
        return 2

    source_path = Path(sys.argv[1])
    source = source_path.read_text(encoding="utf-8")
    start = source.find(CHILD_START)
    end = source.find(CHILD_END, start + len(CHILD_START))
    if start < 0 or end < 0:
        print("could not locate the Unix post-fork child branch", file=sys.stderr)
        return 1

    child_path = source[start:end]
    violations = [
        pattern
        for pattern in FORBIDDEN_PATTERNS
        if re.search(pattern, child_path) is not None
    ]
    if violations:
        print("forbidden helper in Unix post-fork child branch:", file=sys.stderr)
        for pattern in violations:
            print(f"  {pattern}", file=sys.stderr)
        return 1

    print(f"Unix post-fork child path audit passed: {source_path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
