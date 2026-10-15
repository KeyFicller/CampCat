#include "app/ui/script_editor_panel.h"

#include "app/ui/ccat_highlight.h"
#include "app/ui/fonts.h"
#include "app/ui/gl_texture.h"
#include "core/script/ccat_bundle.h"
#include "core/script/ccat_graph.h"

#include <imgui.h>
#include <misc/cpp/imgui_stdlib.h>

#include <opencv2/core.hpp>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <ctime>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace {

/// A template crop is often 40x30; blow it up by whole numbers so the pixels
/// can actually be judged, but never past the box a tooltip may take.
constexpr float k_preview_min_side = 96.0F;
constexpr float k_preview_max_side = 360.0F;

/// Room for the template list inside a script window.
constexpr float k_templates_h = 76.0F;

const ImVec4 k_err_col = ImVec4(0.95F, 0.42F, 0.42F, 1.0F);

/// One open script: its text, the disk content it was opened from, and the last
/// thing that happened to it.
struct script_editor {
    std::string name;
    std::filesystem::path main_path;
    /// The bundle root this script lives under. Held here rather than passed to
    /// draw time because the Script page opens windows too, and it has no way
    /// to know which root any given open window belongs to.
    std::filesystem::path dir;
    std::string loaded; ///< what the file held when opened or last saved
    std::string status; ///< last save/reload outcome, shown on the button row
    bool status_error = false;
    bool open = true;
    bool focus = true;
    std::unique_ptr<TextEditor> editor;
};

std::vector<std::unique_ptr<script_editor>> g_editors;

/// The last scan of the bundle directory, refreshed at most once a second.
std::vector<std::string> g_names;
/// Graphs under that directory, and the one the list is showing.
std::vector<std::string> g_graphs;
std::string g_graph;
/// Anchors and edges of `g_graph`, full bundle names. Filled with the scan,
/// and again when the combo changes, so tagging does not run every frame.
/// Actions hang under the anchor bundle they run on.
std::vector<std::string> g_nodes;
std::vector<std::string> g_edges;
std::map<std::string, std::vector<std::string>> g_actions;
double g_next_scan = 0.0;

/// The deletion being confirmed, and -- when `g_delete_error` is set -- the
/// reason there is nothing to confirm.
constexpr char k_delete_popup[] = "Delete script";
std::string g_delete_name;
std::string g_delete_error;
bool g_delete_pending = false;

/// The rename being confirmed. `g_rename_buf` is the name field; when
/// `g_rename_error` is set the popup shows why the rename was refused instead.
constexpr char k_rename_popup[] = "Rename script";
std::string g_rename_name; ///< the bundle being renamed
std::string g_rename_error;
std::vector<std::string>
    g_rename_referrers; ///< shown as the rewrite blast radius
std::string g_rename_buf;
bool g_rename_pending = false;

/// A decoded template, kept between hovers: key is the absolute path.
struct preview_tex {
    GLuint tex = 0;
    int w = 0;
    int h = 0;
    std::filesystem::file_time_type mtime{};
};
std::map<std::string, preview_tex> g_previews;

bool read_text_file(const std::filesystem::path &_path, std::string *_out) {
    std::ifstream in(_path, std::ios::binary);
    if (!in) {
        return false;
    }
    *_out = std::string{std::istreambuf_iterator<char>(in),
                        std::istreambuf_iterator<char>()};
    return true;
}

std::string clock_now() {
    const std::time_t now = std::time(nullptr);
    std::tm local{};
    localtime_r(&now, &local);
    char buf[16];
    std::strftime(buf, sizeof(buf), "%H:%M:%S", &local);
    return buf;
}

/// Split `g_names` into the anchors and edges of `g_graph`.
void classify_graph(const std::filesystem::path &_dir) {
    g_nodes.clear();
    g_edges.clear();
    g_actions.clear();
    if (g_graph.empty()) {
        return;
    }
    const std::string prefix = g_graph + ".";
    for (const std::string &name : g_names) {
        if (name.rfind(prefix, 0) != 0) {
            continue;
        }
        const campcat::ccat_lang::bundle_tag tag =
            campcat::ccat_lang::bundle_tags(_dir, name);
        if (tag.anchor_of == g_graph) {
            g_nodes.push_back(name);
        } else if (tag.edge_of == g_graph) {
            g_edges.push_back(name);
        } else if (tag.action_of == g_graph) {
            g_actions[prefix + tag.action_on].push_back(name);
        }
    }
}

/// Rescans the bundle directory, at most once a second: the set of skills is a
/// low-frequency thing, and a directory walk on every frame is a walk per
/// frame. Previews of files that are gone or were rewritten by the model are
/// dropped.
void scan_scripts(const std::filesystem::path &_dir) {
    const double now = ImGui::GetTime();
    if (now < g_next_scan) {
        return;
    }
    g_next_scan = now + 1.0;

    std::vector<std::string> names = campcat::ccat_lang::bundle_names(_dir);
    if (names != g_names) {
        g_names = std::move(names);
    }
    g_graphs = campcat::ccat_lang::graph_names(_dir);
    if (!g_graph.empty() &&
        std::find(g_graphs.begin(), g_graphs.end(), g_graph) == g_graphs.end()) {
        g_graph.clear();
    }
    if (g_graph.empty() && !g_graphs.empty()) {
        g_graph = g_graphs.front();
    }
    classify_graph(_dir);

    for (auto it = g_previews.begin(); it != g_previews.end();) {
        std::error_code ec;
        const std::filesystem::file_time_type stamp =
            std::filesystem::last_write_time(std::filesystem::path(it->first),
                                             ec);
        if (ec || stamp != it->second.mtime) {
            campcat::app::delete_tex(&it->second.tex, &it->second.w,
                                     &it->second.h);
            it = g_previews.erase(it);
        } else {
            ++it;
        }
    }
}

/// The decoded template at `_path`, reading it on first sight only. Called
/// while drawing, which is where the GL context is alive.
const preview_tex *preview_for(const std::filesystem::path &_path) {
    const std::string key = _path.string();
    const auto found = g_previews.find(key);
    if (found != g_previews.end()) {
        return &found->second;
    }

    preview_tex entry;
    std::error_code ec;
    entry.mtime = std::filesystem::last_write_time(_path, ec);
    // UNCHANGED: a disc template's shape is its alpha, and COLOR would drop it,
    // drawing the pixels the match ignores as if they counted.
    cv::Mat img = cv::imread(key, cv::IMREAD_UNCHANGED);
    if (img.channels() == 1) {
        cv::cvtColor(img, img, cv::COLOR_GRAY2BGR);
    }
    if (!img.empty()) {
        // GL_NEAREST: a magnified crop should show its actual pixels.
        entry.tex = img.channels() == 4
                        ? campcat::app::upload_bgra(img, &entry.w, &entry.h,
                                                    true)
                        : campcat::app::upload_bgr(img, &entry.w, &entry.h,
                                                   true);
    }
    return &g_previews.emplace(key, entry).first->second;
}

ImVec2 preview_size(int _w, int _h) {
    const float longest = static_cast<float>(std::max(_w, _h));
    if (longest <= 0.0F) {
        return ImVec2(0.0F, 0.0F);
    }
    float scale = 1.0F;
    if (longest > k_preview_max_side) {
        scale = k_preview_max_side / longest;
    } else if (longest < k_preview_min_side) {
        scale = std::floor(k_preview_min_side / longest);
    }
    return ImVec2(static_cast<float>(_w) * scale,
                  static_cast<float>(_h) * scale);
}

/// Keyed by the file, not the name: two roots can each hold a bundle of the
/// same name, and those are not the same script.
script_editor *find_editor(const std::filesystem::path &_main_path) {
    for (const std::unique_ptr<script_editor> &entry : g_editors) {
        if (entry->main_path == _main_path) {
            return entry.get();
        }
    }
    return nullptr;
}

void open_editor(const std::filesystem::path &_dir, const std::string &_name) {
    const std::filesystem::path main_path =
        campcat::ccat_lang::bundle_main(_dir, _name);
    if (script_editor *found = find_editor(main_path)) {
        found->open = true;
        found->focus = true;
        return;
    }

    auto entry = std::make_unique<script_editor>();
    entry->name = _name;
    entry->dir = _dir;
    entry->main_path = main_path;
    if (!read_text_file(entry->main_path, &entry->loaded)) {
        entry->status = "could not read " + entry->main_path.string();
        entry->status_error = true;
    }
    entry->editor = std::make_unique<TextEditor>();
    entry->editor->SetLanguageDefinition(campcat::app::ccat_language());
    entry->editor->SetPalette(campcat::app::ccat_palette());
    entry->editor->SetText(entry->loaded);
    g_editors.push_back(std::move(entry));
}

void save_editor(const std::filesystem::path &_dir, script_editor &_e) {
    const std::string text = _e.editor->GetText();
    std::string err;
    switch (campcat::ccat_lang::save_bundle_main(_dir, _e.name, text, _e.loaded,
                                                 &err)) {
    case campcat::ccat_lang::save_status::ok:
        _e.loaded = text;
        _e.status = "saved " + clock_now();
        _e.status_error = false;
        break;
    case campcat::ccat_lang::save_status::parse_error:
    case campcat::ccat_lang::save_status::write_failed:
    case campcat::ccat_lang::save_status::changed_on_disk:
        _e.status = err;
        _e.status_error = true;
        break;
    }
}

void reload_editor(script_editor &_e) {
    std::string text;
    if (!read_text_file(_e.main_path, &text)) {
        _e.status = "could not read " + _e.main_path.string();
        _e.status_error = true;
        return;
    }
    _e.loaded = text;
    _e.editor->SetText(text);
    _e.status = "reloaded " + clock_now();
    _e.status_error = false;
}

void draw_template_preview(const std::filesystem::path &_path,
                           const std::string &_file) {
    const preview_tex *preview = preview_for(_path);
    ImGui::BeginTooltip();
    if (preview->tex == 0) {
        ImGui::TextUnformatted("unreadable");
    } else {
        ImGui::Image(
            static_cast<ImTextureID>(static_cast<uintptr_t>(preview->tex)),
            preview_size(preview->w, preview->h), ImVec2(0.0F, 0.0F),
            ImVec2(1.0F, 1.0F));
    }
    ImGui::Text("%s  %dx%d", _file.c_str(), preview->w, preview->h);
    ImGui::EndTooltip();
}

/// Drag, hover, double-click and the right-click menu for the item just drawn.
void script_row_actions(const std::filesystem::path &_dir,
                        const std::string &_name, std::string *_rename,
                        std::optional<std::string> *_to_delete) {
    // The row is the grab handle for a drag onto the prompt box -- the whole
    // row, because a handle of its own would squeeze the name, as with the
    // menu below. Safe before the hover block: ImGui clears hover while a
    // drag is live, so that block still sees what it saw before.
    if (ImGui::BeginDragDropSource()) {
        ImGui::SetDragDropPayload(k_script_drag_payload, _name.c_str(),
                                  _name.size());
        ImGui::TextUnformatted(_name.c_str());
        ImGui::EndDragDropSource();
    }
    if (ImGui::IsItemHovered()) {
        // Reading the description means opening main.ccat, so it is fetched
        // only while the pointer is on the row, and shown over it rather
        // than in it.
        const std::string description =
            campcat::ccat_lang::bundle_description(_dir, _name);
        if (!description.empty()) {
            ImGui::SetTooltip("%s", description.c_str());
        }
        if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
            open_editor(_dir, _name);
        }
    }
    // Behind a right click: the column is 180 wide and the row is one
    // Selectable, so a button of its own would squeeze the name.
    if (ImGui::BeginPopupContextItem("ctx")) {
        if (ImGui::MenuItem("Rename")) {
            *_rename = _name;
        }
        if (ImGui::MenuItem("Delete")) {
            *_to_delete = _name;
        }
        ImGui::EndPopup();
    }
}

void draw_script_row(const std::filesystem::path &_dir, const std::string &_name,
                     const char *_label, std::string *_rename,
                     std::optional<std::string> *_to_delete) {
    ImGui::PushID(_name.c_str());
    ImGui::Selectable(_label, false, ImGuiSelectableFlags_AllowDoubleClick);
    script_row_actions(_dir, _name, _rename, _to_delete);
    ImGui::PopID();
}

void draw_name_list(const std::filesystem::path &_dir, const char *_id,
                    float _h, const std::vector<std::string> &_names,
                    const std::string &_prefix, std::string *_rename,
                    std::optional<std::string> *_to_delete) {
    ImGui::BeginChild(_id, ImVec2(0.0F, _h));
    if (_names.empty()) {
        ImGui::TextDisabled("none");
    }
    for (const std::string &name : _names) {
        const std::string label =
            name.rfind(_prefix, 0) == 0 ? name.substr(_prefix.size()) : name;
        draw_script_row(_dir, name, label.c_str(), _rename, _to_delete);
    }
    ImGui::EndChild();
}

/// Anchors of the open graph. One with actions is a tree; the child label is
/// the action name, and the drag still carries the full bundle name.
void draw_node_list(const std::filesystem::path &_dir, float _h,
                    const std::string &_prefix, std::string *_rename,
                    std::optional<std::string> *_to_delete) {
    ImGui::BeginChild("graph_nodes", ImVec2(0.0F, _h));
    if (g_nodes.empty()) {
        ImGui::TextDisabled("none");
    }
    for (const std::string &name : g_nodes) {
        const std::string label =
            name.rfind(_prefix, 0) == 0 ? name.substr(_prefix.size()) : name;
        const auto found = g_actions.find(name);
        if (found == g_actions.end() || found->second.empty()) {
            draw_script_row(_dir, name, label.c_str(), _rename, _to_delete);
            continue;
        }
        ImGui::PushID(name.c_str());
        const bool open = ImGui::TreeNodeEx(
            "##node",
            ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_SpanAvailWidth,
            "%s", label.c_str());
        script_row_actions(_dir, name, _rename, _to_delete);
        if (open) {
            for (const std::string &action : found->second) {
                const std::size_t dot = action.rfind('.');
                const std::string action_label =
                    dot == std::string::npos ? action : action.substr(dot + 1);
                draw_script_row(_dir, action, action_label.c_str(), _rename,
                                _to_delete);
            }
            ImGui::TreePop();
        }
        ImGui::PopID();
    }
    ImGui::EndChild();
}

std::optional<std::string> draw_list(const std::filesystem::path &_dir,
                                     float _w, float _h, std::string *_rename) {
    std::optional<std::string> to_delete;
    ImGui::BeginChild("script_list", ImVec2(_w, _h), ImGuiChildFlags_Borders);
    ImGui::SetNextItemWidth(-FLT_MIN);
    if (ImGui::BeginCombo("##llm_graph",
                          g_graph.empty() ? "(no graph)" : g_graph.c_str())) {
        for (const std::string &name : g_graphs) {
            if (ImGui::Selectable(name.c_str(), name == g_graph) &&
                name != g_graph) {
                g_graph = name;
                classify_graph(_dir);
            }
        }
        ImGui::EndCombo();
    }
    ImGui::SeparatorText("Nodes");
    const ImGuiStyle &style = ImGui::GetStyle();
    const float sep_h = ImGui::GetTextLineHeight() +
                        style.SeparatorTextPadding.y * 2.0F +
                        style.ItemSpacing.y;
    const float nodes_h =
        std::max(1.0F, (ImGui::GetContentRegionAvail().y - sep_h) * 0.5F);
    const std::string prefix = g_graph.empty() ? std::string() : g_graph + ".";
    draw_node_list(_dir, nodes_h, prefix, _rename, &to_delete);
    ImGui::SeparatorText("Edges");
    draw_name_list(_dir, "graph_edges", 0.0F, g_edges, prefix, _rename,
                   &to_delete);
    ImGui::EndChild();
    return to_delete;
}

/// The rename prompt. It says up front which scripts the rename will rewrite,
/// and a refusal is shown inside it so the name can be corrected in place.
void draw_rename_modal(const std::filesystem::path &_dir) {
    if (g_rename_pending) {
        ImGui::OpenPopup(k_rename_popup);
        g_rename_pending = false;
    }
    if (!ImGui::BeginPopupModal(k_rename_popup, nullptr,
                                ImGuiWindowFlags_AlwaysAutoResize)) {
        return;
    }

    ImGui::Text("Rename \"%s\" to:", g_rename_name.c_str());
    ImGui::SetNextItemWidth(240.0F);
    if (ImGui::IsWindowAppearing()) {
        ImGui::SetKeyboardFocusHere();
    }
    ImGui::InputText("##new_name", &g_rename_buf);

    if (!g_rename_referrers.empty()) {
        std::string also;
        for (const std::string &referrer : g_rename_referrers) {
            if (!also.empty()) {
                also += ", ";
            }
            also += referrer;
        }
        ImGui::TextDisabled("This also rewrites every script that runs it: %s.",
                            also.c_str());
    }
    if (!g_rename_error.empty()) {
        ImGui::TextWrapped("%s", g_rename_error.c_str());
    }

    if (ImGui::Button("Rename")) {
        // A trailing space is invisible and would leave a directory nobody can
        // type.
        std::string typed = g_rename_buf;
        const std::size_t first = typed.find_first_not_of(" \t");
        const std::size_t last = typed.find_last_not_of(" \t");
        typed = first == std::string::npos
                    ? std::string()
                    : typed.substr(first, last - first + 1);

        g_rename_error.clear();
        std::vector<std::string> rewritten;
        if (campcat::ccat_lang::rename_bundle(_dir, g_rename_name, typed,
                                              &rewritten, &g_rename_error) ==
            campcat::ccat_lang::rename_status::ok) {
            // The window follows its script; its file is the same one, renamed.
            if (script_editor *open = find_editor(
                    campcat::ccat_lang::bundle_main(_dir, g_rename_name))) {
                open->name = typed;
                open->main_path = campcat::ccat_lang::bundle_main(_dir, typed);
            }
            g_next_scan =
                0.0; // show the new name now instead of within a second
            ImGui::CloseCurrentPopup();
            g_rename_name.clear();
            g_rename_referrers.clear();
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel")) {
        ImGui::CloseCurrentPopup();
        g_rename_name.clear();
        g_rename_referrers.clear();
        g_rename_error.clear();
    }
    ImGui::EndPopup();
}

/// The confirmation for a deletion, or -- when the guard refused -- the reason
/// there is nothing to confirm. Decided when the menu item is clicked, so
/// nobody is asked to confirm a deletion that was never going to happen.
void draw_delete_modal(const std::filesystem::path &_dir) {
    if (g_delete_pending) {
        ImGui::OpenPopup(k_delete_popup);
        g_delete_pending = false;
    }
    if (!ImGui::BeginPopupModal(k_delete_popup, nullptr,
                                ImGuiWindowFlags_AlwaysAutoResize)) {
        return;
    }

    if (!g_delete_error.empty()) {
        ImGui::TextWrapped("%s", g_delete_error.c_str());
        if (ImGui::Button("OK")) {
            ImGui::CloseCurrentPopup();
            g_delete_name.clear();
            g_delete_error.clear();
        }
        ImGui::EndPopup();
        return;
    }

    ImGui::Text("Delete \"%s\"?", g_delete_name.c_str());
    ImGui::TextDisabled(
        "This removes the whole %s/ directory, main.ccat and every "
        "template in it.\nIt cannot be undone.",
        g_delete_name.c_str());
    if (ImGui::Button("Delete")) {
        std::string err;
        if (campcat::ccat_lang::delete_bundle(_dir, g_delete_name, &err) ==
            campcat::ccat_lang::delete_status::ok) {
            // Its file is gone, so a Save in that window could only fail.
            if (script_editor *open = find_editor(
                    campcat::ccat_lang::bundle_main(_dir, g_delete_name))) {
                open->open = false;
            }
            g_next_scan = 0.0; // drop the row now instead of within a second
            ImGui::CloseCurrentPopup();
            g_delete_name.clear();
        } else {
            g_delete_error = err;
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel")) {
        ImGui::CloseCurrentPopup();
        g_delete_name.clear();
    }
    ImGui::EndPopup();
}

void draw_editor(script_editor &_e) {
    if (!_e.open) {
        return;
    }
    const std::filesystem::path bundle_dir = _e.main_path.parent_path();
    const bool dirty = _e.editor->GetText() != _e.loaded;
    // The file, not the name, in the id: two roots can hold same-named bundles,
    // and equal ids make ImGui treat two windows as one. (A path containing "#"
    // would split the id here; no such path is expected.)
    const std::string title = "script: " + _e.name + (dirty ? " *" : "") +
                              "###script_editor_" + _e.main_path.string();

    if (_e.focus) {
        ImGui::SetNextWindowFocus();
        _e.focus = false;
    }
    ImGui::SetNextWindowSize(ImVec2(760.0F, 560.0F), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin(title.c_str(), &_e.open)) {
        ImGui::End();
        return;
    }

    // Everything below the code: the resource box and its separator sit
    // directly above the button row, which is the window's footer, and the code
    // takes whatever is left. ImGui advances the cursor by each item's height
    // plus one ItemSpacing, and a separator is its label plus its own padding.
    // The Script separator is drawn first, so its height is already spent.
    const float footer = ImGui::GetFrameHeightWithSpacing();
    const float below = ImGui::GetTextLineHeight() +
                        ImGui::GetStyle().SeparatorTextPadding.y * 2.0F +
                        ImGui::GetStyle().ItemSpacing.y + k_templates_h +
                        footer;
    ImGui::SeparatorText("Script");
    WITH_FONT(mono, { _e.editor->Render("##ccat", ImVec2(0.0F, -below)); });

    // Templates, read-only: the point of hovering is to see what the model cut.
    ImGui::SeparatorText("Resources");
    const std::vector<std::string> templates =
        campcat::ccat_lang::bundle_templates(_e.dir, _e.name);
    ImGui::BeginChild("templates", ImVec2(0.0F, k_templates_h),
                      ImGuiChildFlags_Borders);
    if (templates.empty()) {
        ImGui::TextDisabled("no templates");
    }
    for (const std::string &file : templates) {
        ImGui::Selectable(file.c_str(), false);
        if (ImGui::IsItemHovered()) {
            draw_template_preview(bundle_dir / file, file);
        }
    }
    ImGui::EndChild();

    if (ImGui::Button("Save")) {
        save_editor(_e.dir, _e);
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Write main.ccat (Cmd+S)");
    }
    ImGui::SameLine();
    if (ImGui::Button("Reload")) {
        reload_editor(_e);
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Drop the changes here and read the file again");
    }
    if (!_e.status.empty()) {
        ImGui::SameLine();
        if (_e.status_error) {
            ImGui::TextColored(k_err_col, "%s", _e.status.c_str());
        } else {
            ImGui::TextDisabled("%s", _e.status.c_str());
        }
    }

    // Cmd on macOS, Ctrl elsewhere; ImGui maps the platform modifier onto Ctrl.
    if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) &&
        ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_S)) {
        save_editor(_e.dir, _e);
    }
    ImGui::End();
}

} // namespace

void script_editor_open(const std::filesystem::path &_script_dir,
                        std::string_view _name) {
    open_editor(_script_dir, std::string(_name));
}

void script_editor_draw_windows() {
    for (const std::unique_ptr<script_editor> &entry : g_editors) {
        draw_editor(*entry);
    }
    // Windows the user closed go away with their editor, along with its undo.
    g_editors.erase(
        std::remove_if(
            g_editors.begin(), g_editors.end(),
            [](const std::unique_ptr<script_editor> &_e) { return !_e->open; }),
        g_editors.end());
}

void script_editor_panel_draw(const std::filesystem::path &_script_dir,
                              float _list_w, float _list_h) {
    scan_scripts(_script_dir);
    std::string rename_clicked;
    if (const std::optional<std::string> clicked =
            draw_list(_script_dir, _list_w, _list_h, &rename_clicked)) {
        g_delete_name = *clicked;
        g_delete_error.clear();
        std::string err;
        // Asked before the popup opens: a refusal should be the first thing the
        // person sees, not something they reach by confirming.
        if (campcat::ccat_lang::check_bundle_deletable(_script_dir,
                                                       g_delete_name, &err) !=
            campcat::ccat_lang::delete_status::ok) {
            g_delete_error = err;
        }
        g_delete_pending = true;
    }
    if (!rename_clicked.empty()) {
        g_rename_name = rename_clicked;
        // Which scripts would be rewritten is known before a name is, so the
        // prompt can say what is at stake while it is being typed.
        g_rename_referrers =
            campcat::ccat_lang::bundle_referrers(_script_dir, rename_clicked);
        g_rename_error.clear();
        g_rename_buf = rename_clicked;
        g_rename_pending = true;
    }
    draw_delete_modal(_script_dir);
    draw_rename_modal(_script_dir);
    script_editor_draw_windows();
}

void script_editor_panel_shutdown_gl() {
    for (auto &pair : g_previews) {
        campcat::app::delete_tex(&pair.second.tex, &pair.second.w,
                                 &pair.second.h);
    }
    g_previews.clear();
    g_editors.clear();
}
