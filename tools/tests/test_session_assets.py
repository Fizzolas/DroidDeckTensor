import importlib.util
from pathlib import Path
import tempfile
import unittest
from zipfile import ZipFile


spec = importlib.util.spec_from_file_location(
    'check_session_assets', Path(__file__).resolve().parents[1] / 'release/check_session_assets.py')
assets = importlib.util.module_from_spec(spec)
spec.loader.exec_module(assets)


class SessionAssetsTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.overlay = self.root / 'overlay'
        self.helper = self.overlay / 'usr/local/bin/droiddeck-login1'
        self.helper.parent.mkdir(parents=True)
        self.helper.write_bytes(b'current helper')
        self.apk = self.root / 'app.apk'
        self.name = 'assets/linuxfs/usr/local/bin/droiddeck-login1'

    def test_missing_required_helper_rejects_cached_bundle(self):
        with ZipFile(self.apk, 'w') as package:
            package.writestr('classes.dex', b'classes')
        self.assertEqual(['missing ' + self.name], assets.check(self.apk, self.overlay))

    def test_stale_helper_rejects_cached_bundle(self):
        with ZipFile(self.apk, 'w') as package:
            package.writestr(self.name, b'old helper')
        self.assertEqual(['stale ' + self.name], assets.check(self.apk, self.overlay))

    def test_current_helper_passes(self):
        with ZipFile(self.apk, 'w') as package:
            package.writestr(self.name, self.helper.read_bytes())
        self.assertEqual([], assets.check(self.apk, self.overlay))

    # A CRLF checkout of a bash helper used to reach the guest and die there with an unreadable
    # "syntax error near unexpected token `$'\r''" - the session reported only "stopped unexpectedly".
    # Both the source on disk and the copy staged into the APK are checked, since a bundle built
    # on a Windows machine before .gitattributes could contain either.

    def test_crlf_helper_source_is_rejected(self):
        self.helper.write_bytes(b'#!/bin/bash\r\necho hi\r\n')
        with ZipFile(self.apk, 'w') as package:
            package.writestr(self.name, self.helper.read_bytes())
        self.assertEqual(['CRLF usr/local/bin/droiddeck-login1'], assets.check(self.apk, self.overlay))

    def test_crlf_surviving_only_in_the_staged_asset_is_caught_as_stale(self):
        with ZipFile(self.apk, 'w') as package:
            package.writestr(self.name, b'#!/bin/bash\r\necho hi\r\n')
        self.assertEqual(['stale ' + self.name], assets.check(self.apk, self.overlay))

    def test_a_binary_helper_with_cr_bytes_is_not_a_line_ending_error(self):
        self.helper.write_bytes(b'\x7fELF\x02\x01\x01\x00\r\n\x00binary')
        with ZipFile(self.apk, 'w') as package:
            package.writestr(self.name, self.helper.read_bytes())
        self.assertEqual([], assets.check(self.apk, self.overlay))
