#!/usr/bin/env python3
"""Check that every hunk of every patch states its real line counts.

A unified diff hunk begins with `@@ -old_start,old_lines +new_start,new_lines @@`
and the tool that applies the patch stops once those counts are satisfied.  A
hunk that under-counts therefore silently drops its last line: that is how the
pong_ramoops_console.h hunk lost the closing brace of pong_ramoops_console_init
in run 43179421190, which only a 20-minute kernel compile reported.

Usage:
    python3 scripts/check-xnu-patches.py [--quiet] [patch ...]

With no patch arguments every patch under patches/ is checked.
"""
import re
import sys
from pathlib import Path

HUNK = re.compile(r'^@@ -(\d+)(?:,(\d+))? \+(\d+)(?:,(\d+))? @@')


def is_file_header(lines, index):
    """True when lines[index] starts the next file of a multi-file diff."""
    if not lines[index].startswith('--- '):
        return False
    return index + 1 < len(lines) and lines[index + 1].startswith('+++ ')


def check(path):
    problems = []
    lines = path.read_text(errors='replace').splitlines()
    index = 0
    while index < len(lines):
        match = HUNK.match(lines[index])
        if not match:
            index += 1
            continue
        header_line = index + 1
        want_old = int(match.group(2)) if match.group(2) is not None else 1
        want_new = int(match.group(4)) if match.group(4) is not None else 1
        index += 1
        old = new = 0
        while index < len(lines) and (old < want_old or new < want_new):
            line = lines[index]
            if line.startswith('\\'):
                index += 1
                continue
            if line.startswith('+') and not line.startswith('+++ '):
                new += 1
            elif line.startswith('-') and not line.startswith('--- '):
                old += 1
            elif line.startswith(' ') or line == '':
                # A hand-written patch can lose the leading space of a context
                # line; an empty line is still a context line here.
                old += 1
                new += 1
            else:
                break
            index += 1
        if old != want_old or new != want_new:
            problems.append('%s:%d: hunk says -%d/+%d, the patch ends at -%d/+%d'
                            % (path, header_line, want_old, want_new, old, new))
            continue
        # The counts are satisfied; anything left that is still hunk content was
        # counted wrong above.
        while index < len(lines):
            line = lines[index]
            if line == '' or line.startswith('\\'):
                index += 1
                continue
            if line.startswith('@@') or line.startswith('diff --git') or is_file_header(lines, index):
                break
            problems.append('%s:%d: hunk ends at -%d/+%d but carries one more line: %r'
                            % (path, header_line, want_old, want_new, line))
            index += 1
    return problems


def main(argv):
    quiet = '--quiet' in argv
    paths = [Path(a) for a in argv[1:] if not a.startswith('-')]
    if not paths:
        paths = sorted(Path('patches').rglob('*.patch'))
    problems = []
    for path in paths:
        problems.extend(check(path))
    for problem in problems:
        print(problem)
    if not quiet or problems:
        print('%d patches checked, %d hunk count problems' % (len(paths), len(problems)))
    return 1 if problems else 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
