"""Process-level checks for the Steam Deck installer using synthetic inputs only."""

import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import time
import unittest


ROOT = Path(__file__).resolve().parents[1]
DEFAULT_BINARY = (
    ROOT
    / "tools"
    / "deck_installer"
    / "target"
    / "x86_64-unknown-linux-musl"
    / "release"
    / "darkrecomp-deck-installer"
)
BINARY = Path(os.environ.get("DECK_INSTALLER_BINARY", DEFAULT_BINARY)).expanduser().resolve()

FAKE_DIALOG = r'''#!/usr/bin/env python3
import json
import os
from pathlib import Path
import sys
import time

args = sys.argv[1:]
name = Path(sys.argv[0]).name
record = {"name": name, "args": args, "pid": os.getpid()}
with open(os.environ["DECK_DIALOG_RECORDS"], "a", encoding="utf-8") as stream:
    stream.write(json.dumps(record) + "\n")

if "--progress" in args:
    Path(os.environ["DECK_PROGRESS_PID"]).write_text(str(os.getpid()), encoding="ascii")
    # The installer owns this child and must shut it down after setup finishes.
    while sys.stdin.buffer.read(1):
        pass
    Path(os.environ["DECK_PROGRESS_CLOSED"]).write_text("closed", encoding="ascii")
    raise SystemExit(0)

if "--progressbar" in args:
    print(":1.42 /MainApplication")
    raise SystemExit(0)

is_picker = "--getexistingdirectory" in args or "--file-selection" in args
is_confirmation = "--yesno" in args or "--question" in args
if is_picker or is_confirmation:
    state_path = Path(os.environ["DECK_DIALOG_STATE"])
    index = int(state_path.read_text(encoding="ascii")) if state_path.exists() else 0
    responses = json.loads(Path(os.environ["DECK_DIALOG_RESPONSES"]).read_text(encoding="utf-8"))
    response = responses[index] if index < len(responses) else {"code": 0, "stdout": ""}
    state_path.write_text(str(index + 1), encoding="ascii")
    if response.get("stdout"):
        sys.stdout.write(response["stdout"])
    raise SystemExit(response.get("code", 0))

raise SystemExit(0)
'''

FAKE_QDBUS = r'''#!/usr/bin/env python3
import json
import os
import sys
from pathlib import Path

with open(os.environ["DECK_QDBUS_RECORD"], "a", encoding="utf-8") as stream:
    stream.write(json.dumps(sys.argv[1:]) + "\n")
'''

FAKE_SETUP = r'''#!/usr/bin/env python3
import json
import os
from pathlib import Path
import sys

args = sys.argv[1:]
Path(os.environ["DECK_SETUP_CAPTURE"]).write_text(
    json.dumps({"argv": args, "cwd": os.getcwd()}), encoding="utf-8"
)
install = Path(args[args.index("--install-dir") + 1])
install.mkdir(parents=True, exist_ok=True)
# This is an inert test fixture, and the installer must never execute it.
(install / "play-linux.sh").write_text("#!/bin/sh\nexit 93\n", encoding="utf-8")
(install / "play-linux.sh").chmod(0o755)
if os.environ.get("DECK_SETUP_FAIL") == "1":
    print("backend fixture failure", file=sys.stderr)
    raise SystemExit(37)
'''


def executable(path, contents):
    path.write_text(contents, encoding="utf-8")
    path.chmod(0o755)


def desktop_escape_exec(path):
    """Expected Exec argument after Exec quoting and desktop-file string escaping."""
    quoted = ['"']
    for char in str(path):
        if char in '\\"`$':
            quoted.append("\\")
        if char == "%":
            quoted.append("%")
        quoted.append(char)
    quoted.append('"')
    return "".join(quoted).replace("\\", "\\\\")


def desktop_escape_string(value):
    return value.replace("\\", "\\\\").replace("\n", "\\n").replace("\r", "\\r").replace("\t", "\\t")


@unittest.skipUnless(os.name == "posix", "the installer integration binary requires Linux/POSIX")
@unittest.skipUnless(BINARY.is_file() and os.access(BINARY, os.X_OK), f"installer binary not found or executable: {BINARY}")
class DeckInstallerProcessTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix="deck installer integration ")
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.system_temp = self.root / "installer temp"
        self.system_temp.mkdir()

        # Exercise paths that must remain single argv values and safe desktop entries.
        self.source = self.root / 'release with spaces "quotes" $cash `ticks` 80% & source'
        (self.source / "tools").mkdir(parents=True)
        (self.source / "tools" / "setup_linux.py").write_text(FAKE_SETUP, encoding="utf-8")
        (self.source / "Launch.sh").write_text("#!/bin/sh\nexit 0\n", encoding="utf-8")
        binary_dir = self.source / "build_native" / "Release"
        binary_dir.mkdir(parents=True)
        (binary_dir / "DarkRecomp.exe").write_bytes(b"synthetic executable marker")

        self.game = self.root / 'owned dump "quotes" $cash `ticks` 50% & data'
        (self.game / "Content").mkdir(parents=True)
        (self.game / "System").mkdir()
        (self.game / "default.xex").write_bytes(b"synthetic dump marker")

        self.destination_parent = self.root / 'destination "quotes" $cash `ticks` 30% & parent'
        self.destination_parent.mkdir()
        (self.destination_parent / "preserve.txt").write_text("existing user file\n", encoding="utf-8")
        self.install = self.destination_parent / "The-Darkness-Recomp"

        self.umu_archive = self.root / 'UMU archive "quoted" $cash `tick` 10%.tar.gz'
        self.proton_archive = self.root / 'GE Proton archive "quoted" $cash `tick` 20%.tar.gz'
        self.umu_archive.write_bytes(b"synthetic offline archive")
        self.proton_archive.write_bytes(b"synthetic offline archive")

        self.fakebin = self.root / "fake executables"
        self.fakebin.mkdir()
        self.setup_capture = self.root / "backend argv.json"
        self.dialog_records = self.root / "dialog calls.jsonl"
        self.dialog_responses = self.root / "dialog responses.json"
        self.dialog_state = self.root / "dialog response index"
        self.progress_pid = self.root / "progress pid"
        self.progress_closed = self.root / "progress closed"
        self.qdbus_record = self.root / "qdbus call.json"

        self.env = os.environ.copy()
        system_path = os.pathsep.join(path for path in ("/usr/bin", "/bin") if Path(path).is_dir())
        self.env.update(
            {
                "PATH": str(self.fakebin) + os.pathsep + system_path,
                "HOME": str(self.root / "fake home"),
                "TMPDIR": str(self.system_temp),
                "DECK_SETUP_CAPTURE": str(self.setup_capture),
                "DECK_DIALOG_RECORDS": str(self.dialog_records),
                "DECK_DIALOG_RESPONSES": str(self.dialog_responses),
                "DECK_DIALOG_STATE": str(self.dialog_state),
                "DECK_PROGRESS_PID": str(self.progress_pid),
                "DECK_PROGRESS_CLOSED": str(self.progress_closed),
                "DECK_QDBUS_RECORD": str(self.qdbus_record),
            }
        )
        (self.root / "fake home").mkdir()

    def install_dialog(self, name):
        executable(self.fakebin / name, FAKE_DIALOG)

    def configure_responses(self, *responses):
        self.dialog_responses.write_text(json.dumps(responses), encoding="utf-8")
        self.dialog_state.unlink(missing_ok=True)
        self.dialog_records.unlink(missing_ok=True)

    def successful_wizard_responses(self):
        return (
            {"code": 0, "stdout": str(self.game) + "\n"},
            {"code": 0, "stdout": str(self.destination_parent) + "\n"},
            {"code": 0},
        )

    def run_installer(self, *args, binary=BINARY, extra_env=None):
        environment = self.env.copy()
        if extra_env:
            environment.update(extra_env)
        return subprocess.run(
            [str(binary), *map(str, args)],
            cwd=self.root,
            env=environment,
            text=True,
            capture_output=True,
            timeout=30,
            check=False,
        )

    def dialog_calls(self):
        if not self.dialog_records.exists():
            return []
        return [json.loads(line) for line in self.dialog_records.read_text(encoding="utf-8").splitlines()]

    def parent_snapshot(self):
        return {
            path.relative_to(self.destination_parent): (path.stat().st_mode & 0o7777, path.read_bytes())
            for path in self.destination_parent.iterdir()
            if path.is_file()
        }

    def expected_setup_args(self):
        return [
            "--source",
            str(self.source),
            "--game-dir",
            str(self.game),
            "--install-dir",
            str(self.install),
            "--umu-archive",
            str(self.umu_archive),
            "--proton-archive",
            str(self.proton_archive),
            "--skip-download",
        ]

    def assert_no_backend_or_install_change(self, snapshot):
        self.assertFalse(self.setup_capture.exists(), "setup backend must not run")
        self.assertFalse(self.install.exists(), "installer must not create the install folder")
        self.assertEqual(self.parent_snapshot(), snapshot)

    def test_game_picker_cancellation_has_no_install_side_effects(self):
        self.install_dialog("kdialog")
        self.configure_responses({"code": 1})
        snapshot = self.parent_snapshot()

        result = self.run_installer("--source", self.source)

        self.assertEqual(result.returncode, 0, result.stderr)
        self.assert_no_backend_or_install_change(snapshot)
        self.assertEqual(len(self.dialog_calls()), 1)
        self.assertIn("--getexistingdirectory", self.dialog_calls()[0]["args"])

    def test_destination_picker_cancellation_has_no_install_side_effects(self):
        self.install_dialog("kdialog")
        self.configure_responses(
            {"code": 0, "stdout": str(self.game) + "\n"},
            {"code": 1},
        )
        snapshot = self.parent_snapshot()

        result = self.run_installer("--source", self.source)

        self.assertEqual(result.returncode, 0, result.stderr)
        self.assert_no_backend_or_install_change(snapshot)
        calls = self.dialog_calls()
        self.assertEqual(len(calls), 2)
        self.assertTrue(all("--getexistingdirectory" in call["args"] for call in calls))

    def test_confirmation_cancellation_has_no_install_side_effects(self):
        self.install_dialog("kdialog")
        snapshot = self.parent_snapshot()

        for cancel_code in (1, 2):
            with self.subTest(cancel_code=cancel_code):
                self.configure_responses(
                    {"code": 0, "stdout": str(self.game) + "\n"},
                    {"code": 0, "stdout": str(self.destination_parent) + "\n"},
                    {"code": cancel_code},
                )

                result = self.run_installer("--source", self.source)

                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertEqual(result.stderr, "")
                self.assert_no_backend_or_install_change(snapshot)
                calls = self.dialog_calls()
                self.assertEqual(len(calls), 3)
                self.assertIn("--yesno", calls[-1]["args"])
                self.assertFalse(any("--error" in call["args"] for call in calls))

    def test_success_preserves_exact_backend_arguments_and_desktop_escaping(self):
        self.install_dialog("kdialog")
        executable(self.fakebin / "qdbus", FAKE_QDBUS)
        self.configure_responses(*self.successful_wizard_responses())

        result = self.run_installer(
            "--source",
            self.source,
            "--umu-archive",
            self.umu_archive,
            "--proton-archive",
            self.proton_archive,
            "--skip-download",
        )

        self.assertEqual(result.returncode, 0, result.stderr)
        captured = json.loads(self.setup_capture.read_text(encoding="utf-8"))
        self.assertEqual(captured["argv"], self.expected_setup_args())
        self.assertEqual(captured["cwd"], str(self.root))

        launcher = self.install / "Play-The-Darkness.desktop"
        self.assertTrue(launcher.is_file())
        self.assertEqual(launcher.stat().st_mode & 0o777, 0o755)
        contents = launcher.read_text(encoding="utf-8")
        exec_line = f'Exec={desktop_escape_exec(self.install / "play-linux.sh")}'
        path_line = f'Path={desktop_escape_string(str(self.install / "game"))}'
        self.assertIn(exec_line, contents.splitlines())
        self.assertIn(path_line, contents.splitlines())
        self.assertIn("30%%", exec_line)
        self.assertIn("Installation complete.", result.stdout)
        self.assertNotIn("backend fixture failure", result.stdout + result.stderr)

        deadline = time.monotonic() + 3
        while (
            (not self.qdbus_record.exists() or len(self.qdbus_record.read_text(encoding="utf-8").splitlines()) < 2)
            and time.monotonic() < deadline
        ):
            time.sleep(0.02)
        self.assertTrue(self.qdbus_record.exists(), "KDialog progress must be closed over D-Bus")
        qdbus_calls = [
            json.loads(line)
            for line in self.qdbus_record.read_text(encoding="utf-8").splitlines()
        ]
        self.assertEqual(
            qdbus_calls,
            [
                [":1.42", "/MainApplication", "showCancelButton", "false"],
                [":1.42", "/MainApplication", "close"],
            ],
        )

    def test_zenity_fallback_closes_its_progress_process(self):
        self.install_dialog("zenity")
        self.configure_responses(*self.successful_wizard_responses())

        result = self.run_installer(
            "--source",
            self.source,
            "--umu-archive",
            self.umu_archive,
            "--proton-archive",
            self.proton_archive,
            "--skip-download",
        )

        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertTrue(self.setup_capture.exists())
        calls = self.dialog_calls()
        self.assertTrue(all(call["name"] == "zenity" for call in calls))
        self.assertTrue(any("--progress" in call["args"] for call in calls))
        self.assertTrue(self.progress_pid.exists(), "Zenity progress process should start")
        pid = int(self.progress_pid.read_text(encoding="ascii"))
        with self.assertRaises(ProcessLookupError):
            os.kill(pid, 0)
        self.assertTrue((self.install / "Play-The-Darkness.desktop").is_file())

    def test_backend_failure_reports_log_and_never_publishes_shortcut_or_success(self):
        self.install_dialog("kdialog")
        self.configure_responses(*self.successful_wizard_responses())

        result = self.run_installer(
            "--source",
            self.source,
            "--umu-archive",
            self.umu_archive,
            "--proton-archive",
            self.proton_archive,
            "--skip-download",
            extra_env={"DECK_SETUP_FAIL": "1"},
        )

        self.assertEqual(result.returncode, 1)
        self.assertIn("backend fixture failure", result.stderr)
        self.assertNotIn("Installation complete.", result.stdout + result.stderr)
        match = re.search(r"Setup log: (.+)", result.stdout)
        self.assertIsNotNone(match, result.stdout)
        log_path = Path(match.group(1).strip())
        self.assertTrue(log_path.is_file())
        self.assertIn("backend fixture failure", log_path.read_text(encoding="utf-8"))
        self.assertIn(str(log_path), result.stderr)
        self.assertTrue((self.install / "play-linux.sh").is_file(), "fixture simulates partial backend output")
        self.assertFalse((self.install / "Play-The-Darkness.desktop").exists())
        calls = self.dialog_calls()
        self.assertTrue(any("--error" in call["args"] for call in calls))
        self.assertFalse(any("--msgbox" in call["args"] for call in calls))

    def test_incomplete_dump_is_rejected_before_backend_or_install_changes(self):
        incomplete = self.root / "incomplete synthetic dump"
        incomplete.mkdir()
        (incomplete / "Content").mkdir()
        (incomplete / "System").mkdir()
        snapshot = self.parent_snapshot()

        result = self.run_installer(
            "--source",
            self.source,
            "--game-dir",
            incomplete,
            "--install-dir",
            self.install,
            "--yes",
        )

        self.assertEqual(result.returncode, 1)
        self.assertIn("default.xex, Content and System", result.stderr)
        self.assertFalse(self.setup_capture.exists())
        self.assertFalse(self.install.exists())
        self.assertEqual(self.parent_snapshot(), snapshot)
        self.assertFalse(self.dialog_records.exists(), "--yes should not open a GUI")

    def test_bundled_layout_uses_sibling_engine_instead_of_release_tools_script(self):
        appdir = self.root / "synthetic AppDir"
        bundled_binary = appdir / "usr" / "bin" / "darkrecomp-deck-installer"
        release = appdir / "usr" / "share" / "darkrecomp" / "release"
        engine = appdir / "usr" / "share" / "darkrecomp" / "setup_linux.py"
        bundled_binary.parent.mkdir(parents=True)
        bundled_binary.write_bytes(BINARY.read_bytes())
        bundled_binary.chmod(0o755)
        shutil.copytree(self.source, release)
        engine.write_text(FAKE_SETUP, encoding="utf-8")

        poison_capture = self.root / "release tools script was used"
        (release / "tools" / "setup_linux.py").write_text(
            "#!/usr/bin/env python3\n"
            "import os\n"
            "from pathlib import Path\n"
            "Path(os.environ['DECK_POISON_CAPTURE']).write_text('wrong engine')\n"
            "raise SystemExit(87)\n",
            encoding="utf-8",
        )
        environment = {"DECK_POISON_CAPTURE": str(poison_capture)}

        result = self.run_installer(
            "--game-dir",
            self.game,
            "--install-dir",
            self.install,
            "--yes",
            binary=bundled_binary,
            extra_env=environment,
        )

        self.assertEqual(result.returncode, 0, result.stderr)
        captured = json.loads(self.setup_capture.read_text(encoding="utf-8"))
        captured_args = captured["argv"]
        source_value = captured_args[captured_args.index("--source") + 1]
        self.assertEqual(Path(source_value).resolve(), release.resolve())
        self.assertIn("--game-dir", captured_args)
        self.assertEqual(captured_args[captured_args.index("--game-dir") + 1], str(self.game))
        self.assertEqual(captured_args[captured_args.index("--install-dir") + 1], str(self.install))
        self.assertFalse(poison_capture.exists(), "bundled setup must use the sibling engine script")
        self.assertTrue((self.install / "Play-The-Darkness.desktop").is_file())
        self.assertFalse(self.dialog_records.exists(), "--yes should not open a GUI")


if __name__ == "__main__":
    unittest.main()
