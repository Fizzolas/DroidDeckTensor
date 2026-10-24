"""The instrument registry: what each instrument is, how to prove it ran, and what proves it works.

The registry is data so that a new instrument is a table edit, and so that `doctor` and `analyze`
agree on the same list. `validate` is public because the file is hand-edited.
"""
from __future__ import annotations

import tomllib
from pathlib import Path

FIELDS = ('kind', 'build', 'identity', 'calibration', 'cost')

# The calibration kinds `calibrate.py` can actually run. A name outside this set is a typo that
# would otherwise silently mean "never checked".
KINDS = {
    'known-pattern', 'known-fault-address', 'known-mmap', 'known-dlopen',
    'build-signature', 'known-marker', 'none',
}

DEFAULT = Path(__file__).with_name('instruments.toml')


def load(path=None):
    with open(path or DEFAULT, 'rb') as f:
        return tomllib.load(f)


def validate(reg):
    errors = []
    for name, entry in reg.items():
        for field in FIELDS:
            if field not in entry:
                errors.append(f"{name}: missing '{field}'")
        kind = entry.get('calibration')
        if kind is not None and kind not in KINDS:
            errors.append(f'{name}: unknown calibration {kind!r}')
    return errors


def cost_per_minute(reg, names):
    return sum(int(reg[n].get('cost', 0)) for n in names if n in reg)
