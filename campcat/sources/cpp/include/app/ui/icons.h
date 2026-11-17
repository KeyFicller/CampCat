#pragma once

/// Lucide glyphs (lucide-static 1.73.0, `font/codepoints.json`) as UTF-8
/// strings, so an icon concatenates with a label and takes the text colour.
/// Codepoints move between Lucide releases: re-read them when `lucide.ttf` is
/// replaced.
namespace campcat::app::icon {

inline constexpr const char *k_cat = "\uE38C";
inline constexpr const char *k_script = "\uE0C3";   // file-code
inline constexpr const char *k_llm = "\uE412";      // sparkles
inline constexpr const char *k_console = "\uE181";  // terminal
inline constexpr const char *k_graph = "\uE425";    // workflow
inline constexpr const char *k_settings = "\uE154";
inline constexpr const char *k_play = "\uE13C";
inline constexpr const char *k_stop = "\uE167";     // square
inline constexpr const char *k_clock = "\uE087";
inline constexpr const char *k_edit = "\uE1F9";     // pencil
inline constexpr const char *k_send = "\uE04A";     // arrow-up
inline constexpr const char *k_chevron_down = "\uE06D";
inline constexpr const char *k_chevron_right = "\uE06F";
inline constexpr const char *k_more = "\uE0B6";     // ellipsis
inline constexpr const char *k_reload = "\uE149";   // rotate-cw
inline constexpr const char *k_fit = "\uE112";      // maximize
inline constexpr const char *k_focus = "\uE0AC";    // crosshair
inline constexpr const char *k_device = "\uE163";   // smartphone
inline constexpr const char *k_approval = "\uE1FE"; // shield-alert
inline constexpr const char *k_failed = "\uE077";   // circle-alert
inline constexpr const char *k_bot = "\uE1BB";
inline constexpr const char *k_node = "\uE345";     // circle-dot
inline constexpr const char *k_edge = "\uE53E";     // route
inline constexpr const char *k_camera = "\uE064";
inline constexpr const char *k_save = "\uE14D";
inline constexpr const char *k_clear = "\uE28F";    // eraser
inline constexpr const char *k_help = "\uE082";     // circle-help
inline constexpr const char *k_file = "\uE0C0";
inline constexpr const char *k_ok = "\uE226";         // circle-check
inline constexpr const char *k_tool = "\uE1B1";       // wrench
inline constexpr const char *k_index_hide = "\uE21C"; // panel-left-close
inline constexpr const char *k_index_show = "\uE21D"; // panel-left-open

/// Every glyph above: the font loader bakes exactly these codepoints.
inline constexpr const char *k_all[] = {
    k_cat,      k_script,       k_llm,           k_console, k_graph,  k_settings,
    k_play,     k_stop,         k_clock,         k_edit,    k_send,   k_chevron_down,
    k_chevron_right, k_more,    k_reload,        k_fit,     k_focus,  k_device,
    k_approval, k_failed,       k_bot,           k_node,    k_edge,   k_camera,
    k_save,     k_clear,        k_help,          k_file,    k_ok,     k_tool,
    k_index_hide, k_index_show,
};

} // namespace campcat::app::icon
