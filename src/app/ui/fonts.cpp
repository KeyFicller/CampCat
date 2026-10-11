#include "app/ui/fonts.h"

#include <imgui.h>

#include <cassert>
#include <filesystem>
#include <system_error>
#include <vector>

namespace campcat::app {
namespace {

/// The bundled faces, relative to the working directory the app is started from
/// (the same base `main.cpp` resolves `config/default.json` against).
constexpr char k_latin_regular[] = "assets/fonts/Arimo-Regular.ttf";
constexpr char k_latin_bold[] = "assets/fonts/Arimo-Bold.ttf";
constexpr char k_mono_regular[] = "assets/fonts/JetBrainsMono-Regular.ttf";

/// Indexed by `font_role`. Written once by `fonts_init`, read-only after.
ImFont *s_font[4] = {};

ImFont *&slot(font_role _role) { return s_font[static_cast<int>(_role)]; }

} // namespace

void fonts_init() {
    ImGuiIO &io = ImGui::GetIO();

    // Every UI font is one family: a Latin face with the same CJK face merged
    // in so Chinese never lands on a font that lacks it. Sizes and weights stay
    // per-use (16 body, 16 bold, 20 titles).
    //
    // The bundled subset is tried first because it is the only candidate that
    // is actually guaranteed to be there -- the system paths below are the ones
    // this change exists to stop depending on, and their Chinese face lives
    // under an AssetsV2 hash that moves between OS releases. They stay as the
    // last resort: a missing `assets/` should not cost Chinese outright.
    static const struct {
        const char *path;
        int face;
    } cjk_candidates[] = {
        {"assets/fonts/NotoSansSC-Common.ttf", 0},
        {"/System/Library/Fonts/Hiragino Sans GB.ttc", 0},
        {"/System/Library/Fonts/Supplemental/Arial Unicode.ttf", 0},
    };

    // ImGui's ChineseFull set stops at the CJK blocks, so Unicode's Arrows
    // block (→, ←, ⇒) is never asked for and draws as nothing, even though the
    // CJK faces carry the common arrows. Appended to ImGui's own list rather
    // than copied, so it cannot drift from it. `static` because ImGui keeps the
    // pointer until the atlas is built, not just for the call -- and because
    // the initializer reads the live context, it has to stay a local static
    // here rather than move to namespace scope.
    //
    // Asking for a codepoint the face does not carry is harmless: the subset in
    // `assets/fonts/` has the common arrows, not every one of the 112 slots.
    static const std::vector<ImWchar> cjk_ranges = [&io] {
        std::vector<ImWchar> r;
        for (const ImWchar *p = io.Fonts->GetGlyphRangesChineseFull();
             p[0] != 0 && p[1] != 0; p += 2) {
            r.push_back(p[0]);
            r.push_back(p[1]);
        }
        r.push_back(0x2190); // Arrows
        r.push_back(0x21FF);
        r.push_back(0); // zero-terminated, as ImGui requires
        r.push_back(0);
        return r;
    }();

    // MergeMode appends to the font added just before it, so each merge is
    // issued right after its own base font.
    //
    // `_cjk` is false for the 20px title only: the full set at 20px doubles the
    // atlas to 4096x16384 (+320 MB RSS, +128 MB VRAM) for strings that are all
    // ASCII today. A Chinese title would draw "?"; add the merge then if it
    // becomes worth that.
    auto load_font = [&io](const char *_path, float _size, bool _cjk,
                           int _face) -> ImFont * {
        // AddFontFromFileTTF asserts on a missing file, and that assert is live
        // in a Debug build. Probing first turns a missing face file into a null
        // return the callers below already know how to fall back from.
        std::error_code ec;
        if (!std::filesystem::exists(_path, ec)) {
            return nullptr;
        }
        ImFontConfig base;
        base.FontNo = _face; // a .ttc holds several faces
        ImFont *font = io.Fonts->AddFontFromFileTTF(_path, _size, &base);
        if (font == nullptr || !_cjk) {
            return font;
        }
        for (const auto &cjk : cjk_candidates) {
            std::error_code cjk_ec;
            if (!std::filesystem::exists(cjk.path, cjk_ec)) {
                continue;
            }
            ImFontConfig merge;
            merge.MergeMode = true;  // add to that font instead of replacing it
            merge.FontNo = cjk.face; // a .ttc holds several faces
            io.Fonts->AddFontFromFileTTF(cjk.path, _size, &merge,
                                         cjk_ranges.data());
            break;
        }
        return font;
    };

    // Arimo for Latin: metric-compatible with Arial, so nothing reflows, and
    // OFL, unlike Arial itself. San Francisco would be unusable anyway: it is a
    // variable font and ImGui's rasterizer ignores variation axes, so every
    // weight -- titles included -- would draw Regular. That is also why the
    // bundled faces are pre-instanced; see `tools/fetch_fonts.py`.
    ImFont *font_body = load_font(k_latin_regular, 16.0F, true, 0);
    if (font_body == nullptr) {
        font_body = io.Fonts->AddFontDefault();
    }
    ImFont *font_bold = load_font(k_latin_bold, 16.0F, true, 0);
    if (font_bold == nullptr) {
        font_bold = font_body;
    }
    // The only Latin weight the CJK merge is skipped for, so a Chinese title
    // would draw "?".
    ImFont *font_title = load_font(k_latin_bold, 20.0F, false, 0);
    if (font_title == nullptr) {
        font_title = font_bold;
    }

    // The script editor is a character grid, so it needs one width for every
    // glyph: with a proportional face the caret and the selection drift off the
    // text. CJK is merged in for the same reason as the other faces -- comments
    // in a script are written in whatever language its author thinks in. The
    // subset costs ~2 MB of atlas, which is why it is a subset at all.
    ImFont *font_mono = load_font(k_mono_regular, 16.0F, true, 0);

    slot(font_role::body) = font_body;
    slot(font_role::bold) = font_bold;
    slot(font_role::title) = font_title;
    // Without the mono face the editor wants the normal one, not nothing: it
    // used to draw in whatever was current, which is `body` -- the default
    // ImGui picks is the first face added, and `body` is it.
    slot(font_role::mono) = font_mono != nullptr ? font_mono : font_body;
}

ImFont *fonts_get(font_role _role) {
    ImFont *f = slot(_role);
    assert(f != nullptr && "fonts_init() must run before the first frame");
    return f != nullptr ? f : ImGui::GetFont();
}

} // namespace campcat::app
