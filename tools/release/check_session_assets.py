"""Reject an APK with missing, stale or CRLF-mangled session helpers, including cached local bundles."""
from pathlib import Path
import sys
from zipfile import ZipFile


OVERLAY = Path(__file__).resolve().parents[1] / 'linuxfs/overlay'


def _crlf_error(name):
    """A guest file that carries CRLF. See the note on [check]."""
    return 'CRLF ' + name


def check_line_endings(overlay=OVERLAY):
    """Every text file under the overlay, free of CR.

    The overlay is bind-mounted into the Linux guest and executed there. A CRLF checkout (a Windows
    machine with the default `core.autocrlf=true` and no `.gitattributes`) therefore produces a
    guest whose bash scripts are one long syntax error: bash reports
    "syntax error near unexpected token `$'\\r''" and exits 2, and the app shows that as
    "<name> stopped unexpectedly" with no hint of the real cause. It cost a day of PowerVR bring-up
    to find, so the build refuses to ship one. The repository-wide rule lives in `.gitattributes`;
    this catches a bundle which was built before it or staged by hand.
    """
    errors = []
    for source in sorted(overlay.rglob('*')):
        if not source.is_file():
            continue
        data = source.read_bytes()
        if b'\x00' in data[:8192]:  # a compiled helper or a blob, not a script
            continue
        if b'\r\n' in data:
            errors.append(_crlf_error(source.relative_to(overlay).as_posix()))
    return errors


def check(apk, overlay=OVERLAY):
    sources = list((overlay / 'usr/local/bin').glob('droiddeck-*'))
    sources += [path for path in [overlay / 'usr/local/bin/steam-compatibility'] if path.is_file()]
    sources += [path for path in (overlay / 'usr/bin').rglob('*') if path.is_file()]
    errors = list(check_line_endings(overlay))
    with ZipFile(apk) as package:
        for source in sources:
            asset = 'assets/linuxfs/' + source.relative_to(overlay).as_posix()
            try:
                content = package.read(asset)
            except KeyError:
                errors.append('missing ' + asset)
                continue
            if content != source.read_bytes():
                errors.append('stale ' + asset)
    return errors


if __name__ == '__main__':
    errors = check(sys.argv[1])
    if errors:
        sys.exit('\n'.join(errors))
    print('APK session helpers match source')
