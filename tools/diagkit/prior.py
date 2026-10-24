"""Search the evidence already on disk before writing a hypothesis about it.

State doc §73: a hypothesis was written up and then refuted by a log line that had already been
pulled, in a file already in the tree. This is that check, mechanised, so it costs one command
instead of one section. Text extensions only - a binary blob is not evidence anyone can cite.
"""
from __future__ import annotations

from pathlib import Path

TEXT_SUFFIXES = {
    '.log', '.txt', '.json', '.jsonl', '.md', '.tsv', '.csv', '.xml', '.conf', '.ini',
    '.sh', '.py', '.c', '.h', '.kt', '.java', '.toml', '.yaml', '.yml',
}
SKIP_DIRS = {'.git', '__pycache__', 'build', '.gradle', '.kotlin', 'node_modules', '.cxx'}


def index(roots):
    found = []
    for root in roots:
        root = Path(root)
        if root.is_file():
            found.append(root)
            continue
        if not root.exists():
            continue
        for path in root.rglob('*'):
            if not path.is_file() or path.suffix.lower() not in TEXT_SUFFIXES:
                continue
            if any(part in SKIP_DIRS for part in path.parts):
                continue
            found.append(path)
    return found


def search(roots, question, limit=50):
    terms = [t.lower() for t in question.split() if t]
    if not terms:
        return []
    hits = []
    for path in index(roots):
        try:
            lines = path.read_text(errors='replace').splitlines()
        except OSError:
            continue
        for number, line in enumerate(lines, 1):
            low = line.lower()
            score = sum(1 for t in terms if t in low)
            if score:
                hits.append({'artifact': path.name, 'path': str(path),
                             'line': number, 'text': line.strip()[:400], 'score': score})
    hits.sort(key=lambda h: (-h['score'], h['artifact'], h['line']))
    return hits[:limit]
