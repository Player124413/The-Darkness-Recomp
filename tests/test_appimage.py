"""Validate the Steam Deck AppImage release boundary without owned game data."""
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import shutil
import stat
import subprocess
import tempfile
import unittest
from unittest.mock import patch
import warnings
import zipfile


ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('package_appimage', ROOT / 'tools/package_appimage.py')
appimage = importlib.util.module_from_spec(spec)
spec.loader.exec_module(appimage)


def elf(machine=62):
    header = bytearray(64)
    header[:7] = b'\x7fELF\x02\x01\x01'
    header[16:18] = (2).to_bytes(2, 'little')
    header[18:20] = machine.to_bytes(2, 'little')
    return bytes(header) + b'fixture executable'


class AppImageTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix='dark appimage & test! ')
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.files = {name: f'fixture: {name}\n'.encode() for name in appimage.REQUIRED_FILES}
        self.files['Launch.sh'] = b'#!/bin/sh\nexit 0\n'
        self.archive = self.root / 'Windows release.zip'
        self.installer = self.root / 'installer with spaces'
        self.installer.write_bytes(elf())
        self.runtime = self.root / 'local runtime'
        self.runtime.write_bytes(elf())
        self.tool = self.root / 'local appimagetool'
        self.tool.write_bytes(b'locally verified tool fixture')
        self.output = self.root / 'release output'
        self.write_release()

    def write_release(self, *, version='v1.2.3', files=None, hashes=None, extras=(), manifest=None):
        files = self.files if files is None else files
        if hashes is None:
            hashes = {name: hashlib.sha256(data).hexdigest() for name, data in files.items()}
        if manifest is None:
            manifest = json.dumps({'version': version, 'commit': 'fixture', 'sha256': hashes}).encode()
        with warnings.catch_warnings():
            warnings.simplefilter('ignore', UserWarning)
            with zipfile.ZipFile(self.archive, 'w', zipfile.ZIP_DEFLATED) as bundle:
                for name, data in files.items():
                    bundle.writestr(name, data)
                bundle.writestr(appimage.PLACEHOLDER, 'Bring your own game files.\n')
                bundle.writestr('RELEASE.json', manifest)
                for name, data in extras:
                    bundle.writestr(name, data)

    def validate(self):
        with zipfile.ZipFile(self.archive) as bundle:
            return appimage.validate_release(bundle)

    def test_release_is_copied_exactly_with_native_appdir_entrypoint(self):
        destination = self.root / 'installer AppDir'
        manifest = appimage.prepare_appdir(self.archive, self.installer, destination)
        self.assertEqual(manifest['version'], 'v1.2.3')
        release = destination / 'usr/share/darkrecomp/release'
        for name, data in self.files.items():
            self.assertEqual((release / name).read_bytes(), data)
        self.assertEqual((destination / 'usr/bin/darkrecomp-deck-installer').read_bytes(), elf())
        shared = destination / 'usr/share/darkrecomp'
        self.assertEqual((shared / 'setup_linux.py').read_bytes(), (ROOT / 'tools/setup_linux.py').read_bytes())
        metadata = json.loads((shared / 'INSTALLER.json').read_text())
        self.assertEqual(metadata['release_version'], 'v1.2.3')
        self.assertEqual(metadata['release_zip_sha256'], hashlib.sha256(self.archive.read_bytes()).hexdigest())
        self.assertEqual(metadata['installer_sha256'], hashlib.sha256(self.installer.read_bytes()).hexdigest())
        self.assertEqual(metadata['sha256']['setup_linux.py'],
                         hashlib.sha256((shared / 'setup_linux.py').read_bytes()).hexdigest())
        self.assertIn(b'exec "$appdir/usr/bin/darkrecomp-deck-installer" "$@"',
                      (destination / 'AppRun').read_bytes())
        self.assertNotIn(b'\r\n', (destination / 'AppRun').read_bytes())
        self.assertIn('Terminal=false', (destination / 'darkrecomp-installer.desktop').read_text())
        self.assertEqual((destination / '.DirIcon').read_bytes(),
                         (destination / 'darkrecomp-installer.svg').read_bytes())
        self.assertEqual(sorted(path.name for path in (release / 'Darkness').iterdir()),
                         ['PUT_GAME_FILES_HERE.txt'])
        if os.name != 'nt':
            self.assertEqual(stat.S_IMODE((destination / 'AppRun').stat().st_mode), 0o755)
            self.assertEqual(stat.S_IMODE((release / 'Launch.sh').stat().st_mode), 0o755)

    def test_entire_release_contract_from_windows_packager_is_required(self):
        for name in ('tools/setup_linux.py', 'Launch.sh',
                     'build_native/Release/DarkRecomp.exe', 'ThirdParty/audio/COPYING.LGPLv2.1'):
            with self.subTest(name=name):
                files = dict(self.files)
                del files[name]
                self.write_release(files=files)
                with self.assertRaisesRegex(appimage.PackageError, 'Incomplete Windows release'):
                    self.validate()

    def test_wizard_corresponding_source_and_license_are_complete_and_hashed(self):
        destination = self.root / 'source AppDir'
        appimage.prepare_appdir(self.archive, self.installer, destination)
        shared = destination / 'usr/share/darkrecomp'
        source = shared / 'installer-source'
        expected = {
            'Cargo.toml': ROOT / 'tools/deck_installer/Cargo.toml',
            'Cargo.lock': ROOT / 'tools/deck_installer/Cargo.lock',
            'src/main.rs': ROOT / 'tools/deck_installer/src/main.rs',
            'COPYING': ROOT / 'COPYING',
        }
        self.assertEqual({path.relative_to(source).as_posix() for path in source.rglob('*') if path.is_file()},
                         {*expected, 'BUILD.txt'})
        for name, origin in expected.items():
            self.assertEqual((source / name).read_bytes(), origin.read_bytes())
        instructions = (source / 'BUILD.txt').read_text()
        self.assertIn('cargo build --release --locked --target x86_64-unknown-linux-musl', instructions)
        self.assertIn('Rust 1.74 or newer', instructions)
        self.assertIn('musl C linker', instructions)
        metadata = json.loads((shared / 'INSTALLER.json').read_text())
        self.assertEqual(set(metadata['sha256']),
                         {'setup_linux.py', *(f'installer-source/{name}' for name in (*expected, 'BUILD.txt'))})
        for name, digest in metadata['sha256'].items():
            self.assertEqual(hashlib.sha256((shared / name).read_bytes()).hexdigest(), digest)

    def test_hash_mismatch_is_rejected_before_creating_destination(self):
        hashes = {name: hashlib.sha256(data).hexdigest() for name, data in self.files.items()}
        hashes['tools/setup_linux.py'] = '0' * 64
        self.write_release(hashes=hashes)
        destination = self.root / 'extracted'
        with self.assertRaisesRegex(appimage.PackageError, 'SHA256 mismatch: tools/setup_linux.py'):
            appimage.extract_release(self.archive, destination)
        self.assertFalse(destination.exists())

    def test_unmanifested_known_crt_dll_is_rejected(self):
        self.write_release(extras=[('build_native/Release/concrt140.dll', b'extra DLL')])
        with self.assertRaisesRegex(appimage.PackageError, 'Unmanifested release files'):
            self.validate()

    def test_manifest_cannot_authorize_game_saves_settings_or_arbitrary_code(self):
        for name in ('Darkness/default.xex', 'Darkness/Content/game.pak', 'saves/private.sav',
                     'DarkRecomp.settings.ini', 'tools/extra.py', 'build_native/Release/private.dll'):
            with self.subTest(name=name):
                files = self.files | {name: b'private fixture'}
                self.write_release(files=files)
                with self.assertRaisesRegex(appimage.PackageError, 'Unexpected release file'):
                    self.validate()

    def test_manifest_cannot_add_nonexistent_game_payload(self):
        hashes = {name: hashlib.sha256(data).hexdigest() for name, data in self.files.items()}
        hashes['Darkness/default.xex'] = '0' * 64
        self.write_release(hashes=hashes)
        with self.assertRaisesRegex(appimage.PackageError, 'Unexpected manifest file'):
            self.validate()

    def test_traversal_absolute_windows_and_ambiguous_paths_are_rejected(self):
        for name in ('../escape', '/tmp/escape', 'C:/escape', 'tools\\setup_linux.py',
                     'tools/../escape', './Launch.sh', 'tools//escape'):
            with self.subTest(name=name):
                # Windows ZipInfo normalizes backslashes when constructed;
                # set the serialized filename explicitly to exercise raw ZIPs.
                entry = zipfile.ZipInfo(name)
                entry.filename = name
                self.write_release(extras=[(entry, b'unsafe')])
                with self.assertRaisesRegex(appimage.PackageError, 'Unsafe ZIP (path|filename)'):
                    self.validate()
        self.assertFalse((self.root / 'escape').exists())

    def test_duplicate_and_case_colliding_entries_are_rejected(self):
        for name in ('Launch.sh', 'launch.sh'):
            with self.subTest(name=name):
                self.write_release(extras=[(name, b'collision')])
                with self.assertRaisesRegex(appimage.PackageError, 'Duplicate ZIP path'):
                    self.validate()

    def test_symlinks_special_files_and_reparse_entries_are_rejected(self):
        for mode in (stat.S_IFLNK | 0o777, stat.S_IFIFO | 0o644,
                     stat.S_IFCHR | 0o644):
            with self.subTest(mode=mode):
                entry = zipfile.ZipInfo('tools')
                entry.create_system = 3
                entry.external_attr = mode << 16
                self.write_release(extras=[(entry, b'/outside')])
                with self.assertRaisesRegex(appimage.PackageError, 'links and special files'):
                    self.validate()
        entry = zipfile.ZipInfo('tools/')
        entry.create_system = 0
        entry.external_attr = 0x400 | 0x10
        self.write_release(extras=[(entry, b'')])
        with self.assertRaisesRegex(appimage.PackageError, 'links and special files'):
            self.validate()

    def test_unexpected_empty_directories_are_rejected(self):
        self.write_release(extras=[('saves/', b'')])
        with self.assertRaisesRegex(appimage.PackageError, 'Unexpected release directory'):
            self.validate()

    def test_expected_directory_entries_are_accepted(self):
        self.write_release(extras=[('tools/', b''), ('build_native/', b''), ('Darkness/', b'')])
        self.assertEqual(self.validate()['version'], 'v1.2.3')

    def test_invalid_manifest_and_duplicate_json_keys_are_rejected(self):
        for manifest in (b'{', b'[]', b'{"version":"v1.2.3"}',
                         b'{"version":"v1.2.3","version":"v9.9.9","sha256":{}}'):
            with self.subTest(manifest=manifest):
                self.write_release(manifest=manifest)
                with self.assertRaises(appimage.PackageError):
                    self.validate()

    def test_version_is_derived_from_manifest_and_cannot_escape_output(self):
        for version in ('../../escape', 'v01.2.3', 'v1.2', 'v1.2.3-01', 'v1.2.3/escape', None):
            with self.subTest(version=version):
                self.write_release(version=version)
                with self.assertRaisesRegex(appimage.PackageError, 'version|prerelease'):
                    self.validate()
        self.write_release(version='v1.2.3-rc.1+build.4')
        self.assertEqual(self.validate()['version'], 'v1.2.3-rc.1+build.4')

    def test_windows_and_other_architecture_installer_inputs_are_rejected(self):
        for data in (b'MZ Windows executable', elf(machine=183)):
            self.installer.write_bytes(data)
            with self.assertRaisesRegex(appimage.PackageError, 'Linux x86_64 ELF'):
                appimage.prepare_appdir(self.archive, self.installer, self.root / 'AppDir')
            self.assertFalse((self.root / 'AppDir').exists())

    def test_cargo_build_is_locked_release_musl(self):
        with patch.object(appimage.subprocess, 'run') as run:
            binary = appimage.build_installer()
        run.assert_called_once_with(['cargo', 'build', '--manifest-path',
            str(ROOT / 'tools/deck_installer/Cargo.toml'), '--release', '--locked',
            '--target', 'x86_64-unknown-linux-musl'], cwd=ROOT, check=True)
        self.assertEqual(binary, ROOT / 'tools/deck_installer/target/x86_64-unknown-linux-musl/release/darkrecomp-deck-installer')

    def test_packaging_uses_local_runtime_preserves_payload_and_emits_checksum(self):
        def run(command, *, cwd, env, check):
            self.assertEqual(command[:4], [str(self.tool.resolve()), '--runtime-file',
                str(self.runtime.resolve()), '--no-appstream'])
            self.assertEqual(cwd, ROOT)
            self.assertTrue(check)
            self.assertEqual(env['ARCH'], 'x86_64')
            self.assertEqual(env['APPIMAGE_EXTRACT_AND_RUN'], '1')
            self.assertEqual(env['VERSION'], 'v1.2.3')
            appdir = Path(command[-2])
            self.assertEqual((appdir / 'usr/share/darkrecomp/release/tools/setup_linux.py').read_bytes(),
                             self.files['tools/setup_linux.py'])
            Path(command[-1]).write_bytes(elf() + b'AppImage fixture')
        with patch.object(appimage.subprocess, 'run', side_effect=run) as invoked:
            result = appimage.package(self.archive, self.output, self.tool, self.runtime, self.installer)
        self.assertEqual(invoked.call_count, 1)
        self.assertEqual(result.name, 'The-Darkness-Recomp-v1.2.3-steam-deck-x86_64.AppImage')
        self.assertEqual(result.with_suffix('.AppImage.sha256').read_text(),
                         f'{hashlib.sha256(result.read_bytes()).hexdigest()}  {result.name}\n')

    def test_packaging_failure_does_not_publish_partial_release(self):
        with patch.object(appimage.subprocess, 'run', side_effect=subprocess.CalledProcessError(1, 'tool')):
            with self.assertRaises(subprocess.CalledProcessError):
                appimage.package(self.archive, self.output, self.tool, self.runtime, self.installer)
        self.assertEqual(list(self.output.iterdir()), [])

    def test_existing_release_or_checksum_is_preserved_without_running_tools(self):
        self.output.mkdir()
        image = self.output / 'The-Darkness-Recomp-v1.2.3-steam-deck-x86_64.AppImage'
        for existing in (image, image.with_suffix('.AppImage.sha256')):
            with self.subTest(existing=existing):
                existing.write_bytes(b'previous release')
                with patch.object(appimage.subprocess, 'run') as run:
                    with self.assertRaises(FileExistsError):
                        appimage.package(self.archive, self.output, self.tool, self.runtime, self.installer)
                    run.assert_not_called()
                self.assertEqual(existing.read_bytes(), b'previous release')
                existing.unlink()

    def test_cli_requires_explicit_tool_and_runtime_without_downloading(self):
        with patch.object(appimage.subprocess, 'run') as run:
            with self.assertRaises(SystemExit) as error:
                appimage.main(['--release-zip', str(self.archive)])
            self.assertEqual(error.exception.code, 2)
            run.assert_not_called()

    @unittest.skipIf(os.name == 'nt' or not shutil.which('sh'), 'requires a POSIX shell')
    def test_apprun_from_unrelated_directory_preserves_arguments(self):
        destination = self.root / 'AppDir with spaces'
        appimage.prepare_appdir(self.archive, self.installer, destination)
        binary = destination / 'usr/bin/darkrecomp-deck-installer'
        binary.write_text('#!/bin/sh\nprintf "%s\\0" "$@" > "$CAPTURE"\nexit 37\n')
        binary.chmod(0o755)
        capture = self.root / 'arguments'
        arguments = ['--source', 'spaces & "quotes" $()', '', 'line\nbreak']
        result = subprocess.run([str(destination / 'AppRun'), *arguments], cwd=self.root,
                                env=os.environ | {'CAPTURE': str(capture)}, timeout=10)
        self.assertEqual(result.returncode, 37)
        self.assertEqual(capture.read_bytes().decode().split('\0')[:-1], arguments)


if __name__ == '__main__':
    unittest.main()
