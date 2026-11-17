#include "app/ui/fonts.h"

#if defined(__APPLE__)
#define GL_SILENCE_DEPRECATION
#endif

#include <GLFW/glfw3.h>

#include <imgui.h>
#include <imgui_internal.h>

#include "app/ui/icons.h"
#include "core/automation_log.h"

#include <cassert>
#include <chrono>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

namespace campcat::app {
namespace {

/// The bundled faces, relative to the working directory the app is started from
/// (the same base `main.cpp` resolves `config.json` against).
constexpr char k_latin_regular[] = "campcat/sources/cpp/assets/fonts/Inter-Regular.ttf";
constexpr char k_latin_semibold[] = "campcat/sources/cpp/assets/fonts/Inter-SemiBold.ttf";
constexpr char k_mono_regular[] = "campcat/sources/cpp/assets/fonts/JetBrainsMono-Regular.ttf";
constexpr char k_icon_face[] = "campcat/sources/cpp/assets/fonts/lucide.ttf";

/// How much larger the merged CJK face is rasterized than the Latin one it
/// joins.
///
/// ImGui scales each face by its own hhea box, which lands Noto's hanzi within
/// 3% of a Latin capital -- nominally the same height. On screen they are not:
/// measured off a screenshot, a capital's solid ink runs 16px against a hanzi's
/// 15px, and past a high alpha threshold the capital is still 14px tall while
/// the hanzi has all but vanished. The strokes are simply lighter, and
/// antialiasing eats more of their edge, so a hanzi reads smaller next to a
/// capital. Scaling it up makes the two match where it matters, and the 3%
/// overshoot is what closes the rest of the gap.
constexpr float k_cjk_scale = 1.20F;

/// Faces merged after the bundled CJK subset to fill in what a GB2312 subset
/// cannot draw: every symbol block, Extension A, compatibility ideographs and
/// the hanzi outside GB2312.
///
/// The bundled subset is tried first for the hanzi it does have, because it is
/// the only face guaranteed to be there. ImGui skips a codepoint an earlier
/// merge already provided, so this second face adds only the remainder and
/// nothing is rasterized twice.
///
/// Only a system face carries those glyphs: Arial Unicode has them all bar
/// Extension A and a few rare ideographs. Merged into every CJK face it took
/// the atlas from 4096x4096 to 4096x16384 at 1x -- 256 MB as RGBA -- and 2x
/// density quadruples each glyph, so only the conversation keeps it: that is
/// where a model's symbols and rare hanzi land.
constexpr char k_wide_face[] =
    "/System/Library/Fonts/Supplemental/Arial Unicode.ttf";

/// Lucide draws on the baseline-to-ascender box; this drops the glyph so its
/// centre meets the centre of a capital. In ems of the face it joins.
constexpr float k_icon_offset_ems = 0.10F;

/// Indexed by `font_role`. Written by `fonts_init`, read-only after.
ImFont *s_font[7] = {};

ImFont *&slot(font_role _role) { return s_font[static_cast<int>(_role)]; }

/// Adds every face at `_density` and fills the slots. Atlas not built yet.
void add_faces(float _density) {
    ImGuiIO &io = ImGui::GetIO();

    // Every UI font is one family: a Latin face with the same CJK face merged
    // in so Chinese never lands on a font that lacks it.
    //
    // The bundled subset is tried first because it is the only candidate that
    // is actually guaranteed to be there -- the system paths below are the ones
    // this change exists to stop depending on, and their Chinese face lives
    // under an AssetsV2 hash that moves between OS releases. They stay as the
    // last resort: a missing `campcat/sources/cpp/assets/` should not cost Chinese outright.
    static const struct {
        const char *path;
        int face;
    } cjk_candidates[] = {
        {"campcat/sources/cpp/assets/fonts/NotoSansSC-Common.ttf", 0},
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
    // `campcat/sources/cpp/assets/fonts/` has the common arrows, not every one of the 112 slots.
    //
    // The blocks after it are the ones a model's Chinese actually reaches and
    // ChineseFull leaves out: circled numbers (①②), box drawing, geometric
    // shapes (●■▲◆), symbols and dingbats (★✓✗), letterlike (™℃), number
    // forms (ⅠⅡ), math (≈≤≠), CJK radicals and strokes, Extension A, the tail
    // of the BMP ideographs past 0x9FAF, compatibility ideographs (豈), and
    // the vertical and small form variants. Circled numbers and geometric
    // shapes are what a list in a reply is drawn with, so their absence was
    // showing up as "?" mid-sentence.
    static const std::vector<ImWchar> cjk_ranges = [&io] {
        std::vector<ImWchar> r;
        for (const ImWchar *p = io.Fonts->GetGlyphRangesChineseFull();
             p[0] != 0 && p[1] != 0; p += 2) {
            r.push_back(p[0]);
            r.push_back(p[1]);
        }
        static const ImWchar wide[] = {
            0x2190, 0x21FF, // Arrows
            0x2100, 0x214F, // Letterlike Symbols
            0x2150, 0x218F, // Number Forms
            0x2200, 0x22FF, // Mathematical Operators
            0x2460, 0x24FF, // Enclosed Alphanumerics
            0x2500, 0x259F, // Box Drawing, Block Elements
            0x25A0, 0x25FF, // Geometric Shapes
            0x2600, 0x27BF, // Miscellaneous Symbols, Dingbats
            0x2E80, 0x2FDF, // CJK Radicals Supplement, Kangxi Radicals
            0x31C0, 0x31EF, // CJK Strokes
            0x3400, 0x4DBF, // CJK Extension A
            0x9FB0, 0x9FFF, // the rest of the BMP ideographs
            0xF900, 0xFAFF, // CJK Compatibility Ideographs
            0xFE10, 0xFE6F, // vertical, compatibility and small form variants
        };
        for (unsigned i = 0; i < sizeof(wide) / sizeof(wide[0]); i++) {
            r.push_back(wide[i]);
        }
        r.push_back(0); // zero-terminated, as ImGui requires
        r.push_back(0);
        return r;
    }();

    // The small face is Latin-only. Infinity and the em dash sit outside that
    // range, and ImGui keeps this pointer until the atlas is built.
    static const std::vector<ImWchar> status_ranges = [&io] {
        std::vector<ImWchar> r;
        for (const ImWchar *p = io.Fonts->GetGlyphRangesDefault();
             p[0] != 0 && p[1] != 0; p += 2) {
            r.push_back(p[0]);
            r.push_back(p[1]);
        }
        r.push_back(0x2014); // em dash: unknown context
        r.push_back(0x2014);
        r.push_back(0x221E); // infinity: an unlimited fuse
        r.push_back(0x221E);
        r.push_back(0);
        r.push_back(0);
        return r;
    }();

    // Exactly the glyphs `icon::k_all` names, so the 2k-glyph face costs a
    // few dozen cells.
    static const std::vector<ImWchar> icon_ranges = [] {
        std::vector<ImWchar> r;
        for (const char *glyph : icon::k_all) {
            unsigned int c = 0;
            ImTextCharFromUtf8(&c, glyph, nullptr);
            r.push_back(static_cast<ImWchar>(c));
            r.push_back(static_cast<ImWchar>(c));
        }
        r.push_back(0);
        return r;
    }();

    auto exists = [](const char *_path) {
        std::error_code ec;
        return std::filesystem::exists(_path, ec);
    };

    // MergeMode appends to the font added just before it, so each merge is
    // issued right after its own base font.
    //
    // `_cjk` is false for the Latin-only faces (title, display, small):
    // Chinese draws "?" there, and the atlas does not pay for a CJK merge at
    // every size.
    auto load_font = [&](const char *_path, float _size, bool _cjk, bool _wide,
                         bool _icons, const ImWchar *_ranges = nullptr) -> ImFont * {
        // AddFontFromFileTTF asserts on a missing file, and that assert is live
        // in a Debug build. Probing first turns a missing face file into a null
        // return the callers below already know how to fall back from.
        if (!exists(_path)) {
            return nullptr;
        }
        ImFontConfig base;
        base.RasterizerDensity = _density;
        ImFont *font = io.Fonts->AddFontFromFileTTF(_path, _size, &base, _ranges);
        if (font == nullptr) {
            return font;
        }
        if (_icons && exists(k_icon_face)) {
            ImFontConfig merge;
            merge.MergeMode = true;
            merge.RasterizerDensity = _density;
            merge.GlyphMinAdvanceX = _size;
            merge.GlyphOffset.y = _size * k_icon_offset_ems;
            io.Fonts->AddFontFromFileTTF(k_icon_face, _size, &merge,
                                         icon_ranges.data());
        }
        if (!_cjk) {
            return font;
        }
        for (const auto &cjk : cjk_candidates) {
            if (!exists(cjk.path)) {
                continue;
            }
            ImFontConfig merge;
            merge.MergeMode = true;  // add to that font instead of replacing it
            merge.FontNo = cjk.face; // a .ttc holds several faces
            merge.RasterizerDensity = _density;
            io.Fonts->AddFontFromFileTTF(cjk.path, _size * k_cjk_scale, &merge,
                                         cjk_ranges.data());
            break;
        }
        if (!_wide || !exists(k_wide_face)) {
            return font;
        }
        ImFontConfig fill;
        fill.MergeMode = true;
        fill.RasterizerDensity = _density;
        io.Fonts->AddFontFromFileTTF(k_wide_face, _size * k_cjk_scale, &fill,
                                     cjk_ranges.data());
        return font;
    };

    // Inter for Latin: OFL, and drawn for screens -- its tall x-height is what
    // keeps 13-15px legible. San Francisco would be unusable anyway: it is a variable
    // font and ImGui's rasterizer ignores variation axes, so every weight --
    // titles included -- would draw Regular. That is also why the bundled
    // faces are static instances; see `campcat/sources/cpp/assets/fonts/README.md`.
    ImFont *font_body = load_font(k_latin_regular, 15.0F, true, false, true);
    if (font_body == nullptr) {
        font_body = io.Fonts->AddFontDefault();
    }
    // SemiBold rather than Bold: a page of Bold buttons reads heavy.
    ImFont *font_bold = load_font(k_latin_semibold, 15.0F, true, false, true);
    if (font_bold == nullptr) {
        font_bold = font_body;
    }
    ImFont *font_title = load_font(k_latin_semibold, 20.0F, false, false, true);
    if (font_title == nullptr) {
        font_title = font_bold;
    }
    // Empty-state headlines: Latin only, so the size costs a few hundred cells.
    ImFont *font_display = load_font(k_latin_semibold, 26.0F, false, false, true);
    if (font_display == nullptr) {
        font_display = font_title;
    }

    // The script editor is a character grid, so it needs one width for every
    // glyph: with a proportional face the caret and the selection drift off the
    // text. CJK is merged in for the same reason as the other faces -- comments
    // in a script are written in whatever language its author thinks in. The
    // subset costs ~2 MB of atlas, which is why it is a subset at all.
    ImFont *font_mono = load_font(k_mono_regular, 14.0F, true, false, false);

    // Status bar, stats line and hints: context, not content. Latin-only like
    // `title`; two codepoints outside Latin come along, infinity and the em dash.
    ImFont *font_small = load_font(k_latin_regular, 13.0F, false, false, true,
                                   status_ranges.data());
    if (font_small == nullptr) {
        font_small = font_body;
    }

    // The conversation, one size up from the chrome around it. A real face
    // rather than a window font scale: `imgui_markdown` takes its wrap width
    // from `io.FontGlobalScale`, which `SetWindowFontScale` leaves at 1.0, so a
    // scaled window wraps each reply to its unscaled width and clips the last
    // 15% of every line. It would also stretch the glyphs instead of
    // rasterizing them. Icons too: thinking and failure rows draw inside it.
    ImFont *font_chat = load_font(k_latin_regular, 16.0F, true, true, true);
    if (font_chat == nullptr) {
        font_chat = font_body;
    }

    slot(font_role::body) = font_body;
    slot(font_role::bold) = font_bold;
    slot(font_role::title) = font_title;
    slot(font_role::display) = font_display;
    // Without the mono face the editor wants the normal one, not nothing: it
    // used to draw in whatever was current, which is `body` -- the default
    // ImGui picks is the first face added, and `body` is it.
    slot(font_role::mono) = font_mono != nullptr ? font_mono : font_body;
    slot(font_role::small) = font_small;
    slot(font_role::chat) = font_chat;
}

} // namespace

void fonts_init(float _content_scale) {
    const auto started = std::chrono::steady_clock::now();
    ImGuiIO &io = ImGui::GetIO();
    float density = font_density(_content_scale);
    add_faces(density);
    io.Fonts->Build();

    // A sheet taller than the GPU allows uploads as nothing, and every glyph
    // in the app goes blank with it. 1x is blurry on Retina but always fits.
    GLint max_texture = 0;
    glGetIntegerv(GL_MAX_TEXTURE_SIZE, &max_texture);
    if (density > 1.0F && max_texture > 0 &&
        (io.Fonts->TexHeight > max_texture || io.Fonts->TexWidth > max_texture)) {
        automation_log::emit("[ui] font atlas too large at density " +
                             std::to_string(static_cast<int>(density)) +
                             ", rebuilt at 1");
        io.Fonts->Clear();
        density = 1.0F;
        add_faces(density);
        io.Fonts->Build();
    }

    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - started)
                        .count();
    automation_log::emit("[ui] font atlas " + std::to_string(io.Fonts->TexWidth) +
                         "x" + std::to_string(io.Fonts->TexHeight) + ", density " +
                         std::to_string(static_cast<int>(density)) + ", built in " +
                         std::to_string(ms) + " ms");
}

ImFont *fonts_get(font_role _role) {
    ImFont *f = slot(_role);
    assert(f != nullptr && "fonts_init() must run before the first frame");
    return f != nullptr ? f : ImGui::GetFont();
}

} // namespace campcat::app
