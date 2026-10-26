#include "runtime/native/native_menu_text.h"
#include <cstdio>
#include <initializer_list>
#include <stdexcept>
#include <utility>

using namespace DarkRecomp::Native;
static void check(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
int main() {
    try {
        for (const bool revision : {false, true}) {
            for (const uint32_t language : {1u, 3u, 4u, 5u, 6u}) {
                initializeNativeMenuText(revision, language);
                const bool expected = revision && language == 1;
                check(russianNativeMenus() == expected, "Russian menus escaped their verified revision/content slot");
                check(nativeMenuText("ON") == (expected ? "\xc2\xca\xcb" : "ON"),
                      "Cyrillic menu text was not encoded as CP1251");
                check(nativeMenuText("1440P") == "1440P" && nativeMenuText("MSAA 4X") == "MSAA 4X",
                      "technical labels lost their fallback");
            }
        }
        initializeNativeMenuText(true, 1);
        check(nativeMenuText("MOVE FORWARD") == "\xc2\xcf\xc5\xd0\xa8\xc4",
              "Russian Yo did not retain its font glyph byte");
        initializeNativeMenuText(false, 1);
        check(nativeMenuText("MOVE FORWARD") == "MOVE FORWARD", "Russian state survived a stock reload");
        for (const auto [language, size] : {
                std::pair{1u, std::string_view("SUBTITLE SIZE")},
                std::pair{3u, std::string_view("UNTERTITELGR\xd6SSE")},
                std::pair{4u, std::string_view("TAILLE SOUS-TIT.")},
                std::pair{5u, std::string_view("TAM. SUBT\xcdTULOS")},
                std::pair{6u, std::string_view("DIM. SOTTOTITOLI")}}) {
            initializeNativeMenuText(false, language);
            check(nativeMenuText("SUBTITLE SIZE") == size, "subtitle label ignored the effective game language");
            const auto help = nativeMenuText("LEFT/RIGHT: CHANGE; CONFIRM: NEXT");
            check(!help.empty() && help.size() <= 40, "subtitle help does not fit the menu row");
            check(language == 1 || help != "LEFT/RIGHT: CHANGE; CONFIRM: NEXT", "subtitle help remained English");
            check(nativeMenuText("ON") == "ON" && nativeMenuText("UNRELATED LABEL") == "UNRELATED LABEL",
                  "subtitle translations changed an unrelated PC label");
        }
        initializeNativeMenuText(true, 1);
        check(nativeMenuText("AUTO") == "\xc0\xc2\xd2\xce" &&
              nativeMenuText("4:3 STRETCH") == "4:3 \xd0\xc0\xd1\xd2\xdf\xc6\xca\xc0",
              "aspect values were not translated for the Russian menu");
        check(nativeMenuText("SUBTITLE SIZE") == "\xd0\xc0\xc7\xcc\xc5\xd0 \xd1\xd3\xc1\xd2\xc8\xd2\xd0\xce\xc2",
              "western subtitle translations replaced the Russian revision text");
        initializeNativeMenuText(false, 999);
        check(!russianNativeMenus() && nativeMenuText("SUBTITLE SIZE") == "SUBTITLE SIZE",
              "unsupported language did not use the original English fallback");
        std::puts("Native menu text: verified revision, English content slot, CP1251 and technical fallback passed.");
        return 0;
    } catch (const std::exception& error) { std::fprintf(stderr, "%s\n", error.what()); return 1; }
}
