"""Catalog encoding, layout limits and shared native-menu dictionary contracts."""
from pathlib import Path
import json
import re
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
from compile_native_menu_text import (DEFAULT_CATALOG, DEFAULT_SUBTITLE_CATALOG, SUBTITLE_LANGUAGES,
                                      compile_header, load_catalog, load_subtitle_catalog)
from compile_video_menu import localized_text


class NativeMenuText(unittest.TestCase):
    def test_catalog_and_header_share_exact_cp1251_glyph_bytes(self):
        translations = load_catalog()
        self.assertEqual(translations["BRIGHTNESS"].encode("latin1"), "ЯРКОСТЬ".encode("cp1251"))
        self.assertEqual(translations["MOVE FORWARD"].encode("latin1"), "ВПЕРЁД".encode("cp1251"))
        header = compile_header(translations)
        self.assertTrue(header.isascii(), "generated C++ must not depend on compiler source encoding")
        entries = re.findall(r'\{"([^"\\]+)", "((?:\\x[0-9a-f]{2})+)"\}', header)
        self.assertEqual(len(entries), len(translations))
        for english, escaped in entries:
            encoded = bytes(int(value, 16) for value in re.findall(r"\\x([0-9a-f]{2})", escaped))
            self.assertEqual(encoded, translations[english].encode("latin1"), english)
        self.assertIn("return english;", header, "technical values must retain an English fallback")

    def test_dynamic_settings_and_keyboard_status_have_compact_translations(self):
        translations = load_catalog()
        for text in ("ON", "OFF", "ORIGINAL", "BORDERLESS", "WINDOWED", "UNLIMITED", "SYSTEM",
                     "AUTO", "4:3 STRETCH",
                     "ENGLISH", "GERMAN", "FRENCH", "SPANISH", "ITALIAN", "SAVE FAILED"):
            self.assertIn(text, translations)
            self.assertLessEqual(len(translations[text]), 12, text)
        self.assertLessEqual(len(translations["DISPLAY ASPECT"]), 20)
        self.assertLessEqual(len(translations["RES/ASPECT/LANG: RESTART TO APPLY"]), 40)
        for text in ("UNBOUND", "RELEASE", "PRESSKEY", "PRESS..."):
            self.assertIn(text, translations)
            self.assertLessEqual(len(translations[text]), 8, text)
        for text in ("SAVE APPLIES CHANGES. CANCEL DISCARDS.", "RELEASE HELD KEYS/BUTTONS, THEN PRESS.",
                     "PRESS A KEY/MOUSE; ESC CANCEL, DEL CLEAR", "RESERVED KEY. TRY AGAIN; ESC CANCELS.",
                     "BINDING UPDATED. SAVE TO APPLY.", "DEFAULTS STAGED. SAVE TO APPLY.",
                     "SAVING KEYBOARD/MOUSE BINDINGS...", "SAVE FAILED. TRY AGAIN OR CANCEL.",
                     "KEYBOARD/MOUSE BINDINGS SAVED."):
            self.assertIn(text, translations)
            self.assertLessEqual(len(translations[text]), 40, text)

    def test_subtitle_catalog_and_header_share_cp1252_bytes_and_language_ids(self):
        translations = load_subtitle_catalog()
        self.assertEqual(SUBTITLE_LANGUAGES, {"en": 1, "de": 3, "fr": 4, "es": 5, "it": 6})
        self.assertEqual(set(translations), set(SUBTITLE_LANGUAGES))
        self.assertEqual(translations["de"]["SUBTITLE SIZE"].encode("latin1"),
                         "UNTERTITELGRÖSSE".encode("cp1252"))
        self.assertEqual(translations["es"]["SUBTITLE SIZE"].encode("latin1"),
                         "TAM. SUBTÍTULOS".encode("cp1252"))
        for language, strings in translations.items():
            self.assertEqual(set(strings), {"SUBTITLE SIZE", "LEFT/RIGHT: CHANGE; CONFIRM: NEXT"})
            self.assertLessEqual(len(strings["SUBTITLE SIZE"]), 16, language)
            self.assertLessEqual(len(strings["LEFT/RIGHT: CHANGE; CONFIRM: NEXT"]), 40, language)
        header = compile_header(load_catalog(), translations)
        self.assertTrue(header.isascii(), "Western accents must not depend on the C++ source encoding")
        entries = re.findall(r'\{([13456]), "([^"\\]+)", "((?:\\x[0-9a-f]{2})+)"\}', header)
        self.assertEqual(len(entries), 10, "All five subtitle languages must reach the runtime dictionary")
        language_codes = {str(identifier): language for language, identifier in SUBTITLE_LANGUAGES.items()}
        seen = set()
        for identifier, english, escaped in entries:
            encoded = bytes(int(value, 16) for value in re.findall(r"\\x([0-9a-f]{2})", escaped))
            language = language_codes[identifier]
            self.assertEqual(encoded, translations[language][english].encode("latin1"))
            seen.add((language, english))
        self.assertEqual(len(seen), 10)
        lookup = header.split("constexpr std::string_view subtitleNativeMenuText", 1)[1]
        self.assertIn("entry.language == languageId && entry.english == english", lookup)
        self.assertIn("return english;", lookup, "Unknown languages and non-subtitle text must retain their fallback")
        changed = {language: dict(strings) for language, strings in translations.items()}
        changed["de"]["SUBTITLE SIZE"] = "TITELGRÖSSE".encode("cp1252").decode("latin1")
        self.assertNotEqual(compile_header(load_catalog(), changed), header,
                            "Explicit subtitle catalog was ignored by the header generator")

    def test_subtitle_catalog_rejects_missing_scope_invalid_encoding_and_overflow(self):
        valid = json.loads(DEFAULT_SUBTITLE_CATALOG.read_text(encoding="utf-8"))
        size = "SUBTITLE SIZE"
        help_key = "LEFT/RIGHT: CHANGE; CONFIRM: NEXT"
        cases = (
            ("missing language", lambda d: d["languages"].pop("fr")),
            ("extra language", lambda d: d["languages"].update(ru=d["languages"]["de"])),
            ("missing size", lambda d: d["languages"]["de"].pop(size)),
            ("missing help", lambda d: d["languages"]["es"].pop(help_key)),
            ("extra key", lambda d: d["languages"]["it"].update(OTHER=d["languages"]["it"][size])),
            ("lowercase key", lambda d: d["languages"]["de"].update({size.lower(): d["languages"]["de"].pop(size)})),
            ("lowercase text", lambda d: d["languages"]["de"][size].update(text="Untertitelgröße")),
            ("control character", lambda d: d["languages"]["fr"][size].update(text="TAILLE\n")),
            ("unencodable", lambda d: d["languages"]["de"][size].update(text="😀")),
            ("Cyrillic in Western catalog", lambda d: d["languages"]["de"][size].update(text="РАЗМЕР")),
            ("label overflow", lambda d: d["languages"]["de"][size].update(text="A" * 17)),
            ("help overflow", lambda d: d["languages"]["it"][help_key].update(text="A" * 41)),
            ("expanded row limit", lambda d: d["languages"]["de"][size].update(max_glyphs=20)),
            ("boolean limit", lambda d: d["languages"]["de"][size].update(max_glyphs=True)),
            ("empty text", lambda d: d["languages"]["fr"][size].update(text="")),
            ("changed English source", lambda d: d["languages"]["en"][size].update(text="TEXT SIZE")),
            ("wrong encoding", lambda d: d.update(encoding="utf-8")),
            ("boolean format", lambda d: d.update(format=True)),
        )
        with tempfile.TemporaryDirectory(prefix="DarkRecomp subtitle catalog ") as temporary:
            path = Path(temporary) / "catalog.json"
            for name, mutate in cases:
                source = json.loads(json.dumps(valid))
                mutate(source)
                encoded = json.dumps(source, ensure_ascii=False).encode("utf-8")
                path.write_bytes(encoded)
                with self.subTest(case=name), self.assertRaises(ValueError):
                    load_subtitle_catalog(path)
                self.assertEqual(path.read_bytes(), encoded, "Validation changed the translation source")
            duplicate = DEFAULT_SUBTITLE_CATALOG.read_text(encoding="utf-8").replace(
                '"format": 1', '"format": 1, "format": 1', 1)
            path.write_text(duplicate, encoding="utf-8")
            with self.assertRaisesRegex(ValueError, "Duplicate"):
                load_subtitle_catalog(path)
            path.write_bytes(b'\xff')
            with self.assertRaises(UnicodeDecodeError):
                load_subtitle_catalog(path)

    def test_subtitle_catalog_rejects_cp1252_glyphs_missing_from_the_stock_font(self):
        valid = json.loads(DEFAULT_SUBTITLE_CATALOG.read_text(encoding="utf-8"))
        with tempfile.TemporaryDirectory(prefix="DarkRecomp Western font catalog ") as temporary:
            path = Path(temporary) / "catalog.json"
            for text in ("TEXT “SIZE”", "TAILLE €"):
                source = json.loads(json.dumps(valid))
                source["languages"]["fr"]["SUBTITLE SIZE"]["text"] = text
                path.write_text(json.dumps(source, ensure_ascii=False), encoding="utf-8")
                with self.subTest(text=text), self.assertRaisesRegex(ValueError, "Western font glyph"):
                    load_subtitle_catalog(path)

    def test_catalog_rejects_unencodable_text_and_overflow_instead_of_truncating(self):
        valid = json.loads(DEFAULT_CATALOG.read_text(encoding="utf-8"))
        cases = (("text", "😀"), ("text", "яркость"), ("text", "ТЕСТ\n"),
                 ("text", "Я" * 21), ("max_glyphs", 0), ("max_glyphs", True))
        with tempfile.TemporaryDirectory(prefix="DarkRecomp menu catalog ") as temporary:
            path = Path(temporary) / "catalog.json"
            for field, value in cases:
                source = json.loads(json.dumps(valid))
                source["strings"]["BRIGHTNESS"][field] = value
                path.write_text(json.dumps(source, ensure_ascii=False), encoding="utf-8")
                with self.subTest(field=field, value=value), self.assertRaises(ValueError):
                    load_catalog(path)

    def test_generated_controls_reject_missing_text_and_region_overflow(self):
        translations = load_catalog()
        with self.assertRaisesRegex(ValueError, "Missing Russian"):
            localized_text("sc, NEW SETTING", "1,4,10,1", translations)
        without_initial = dict(translations)
        del without_initial["SYSTEM"]
        with self.assertRaisesRegex(ValueError, "Missing Russian"):
            localized_text("sc, <    SYSTEM    >", "11,4,8,1", without_initial)
        with self.assertRaisesRegex(ValueError, "region"):
            localized_text("nc, KEYBOARD BINDINGS", "0,4,5,1", translations)

    def test_header_cli_is_repeatable_and_never_overwrites_the_catalog(self):
        with tempfile.TemporaryDirectory(prefix="DarkRecomp menu header ") as temporary:
            path = Path(temporary) / "native_menu_text.generated.h"
            command = [sys.executable, str(ROOT / "tools/compile_native_menu_text.py"),
                       "--catalog", str(DEFAULT_CATALOG), "--subtitle-catalog", str(DEFAULT_SUBTITLE_CATALOG),
                       "--output", str(path)]
            subprocess.run(command, check=True, capture_output=True)
            before = path.stat().st_mtime_ns
            subprocess.run(command, check=True, capture_output=True)
            self.assertEqual(path.stat().st_mtime_ns, before, "unchanged catalog rewrote generated header")
            self.assertEqual(path.read_text(encoding="ascii"), compile_header(load_catalog()))
            for catalog in (DEFAULT_CATALOG, DEFAULT_SUBTITLE_CATALOG):
                before_catalog = catalog.read_bytes()
                rejected = subprocess.run(command[:-1] + [str(catalog)], capture_output=True)
                self.assertNotEqual(rejected.returncode, 0)
                self.assertEqual(catalog.read_bytes(), before_catalog)

    def test_header_cli_honors_an_explicit_subtitle_catalog(self):
        source = json.loads(DEFAULT_SUBTITLE_CATALOG.read_text(encoding="utf-8"))
        source["languages"]["de"]["SUBTITLE SIZE"]["text"] = "TITELGRÖSSE"
        with tempfile.TemporaryDirectory(prefix="DarkRecomp subtitle header ") as temporary:
            catalog = Path(temporary) / "subtitle.json"
            output = Path(temporary) / "native_menu_text.generated.h"
            catalog.write_text(json.dumps(source, ensure_ascii=False), encoding="utf-8")
            command = [sys.executable, str(ROOT / "tools/compile_native_menu_text.py"),
                       "--subtitle-catalog", str(catalog), "--output", str(output)]
            subprocess.run(command, check=True, capture_output=True)
            self.assertEqual(output.read_text(encoding="ascii"),
                             compile_header(load_catalog(), load_subtitle_catalog(catalog)))
            self.assertNotEqual(output.read_text(encoding="ascii"), compile_header(load_catalog()))


if __name__ == "__main__":
    unittest.main()
