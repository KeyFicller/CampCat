#include "app/ui/graph_panel.h"

#include "app/ui/fonts.h"
#include "app/ui/gl_texture.h"
#include "app/ui/script_editor_panel.h"
#include "core/app_config.h"
#include "core/script/ccat_bundle.h"
#include "core/script/ccat_graph.h"

#include "GraphEditor.h"

#include <imgui.h>

#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace {

/// The canvas's metrics, in canvas units, at zoom 1. GraphEditor lays a node
/// out in these units and paints a fixed 20px title band across its top, so
/// `k_header_h` is that band plus the 3px the body is inset by; together with
/// the padding they add up to a body that fits its lines exactly.
constexpr float k_node_w = 240.0F;
constexpr float k_header_h = 23.0F;
constexpr float k_text_pad = 8.0F;
constexpr float k_body_pad = 3.0F;
constexpr float k_line_h = 17.0F;
constexpr float k_node_min_h = 72.0F;
constexpr float k_gap_x = 88.0F;
constexpr float k_gap_y = 28.0F;
/// Longest side of a template tooltip, and the floor under which a crop is
/// magnified by a whole number. The same bounds the script editor uses.
constexpr float k_preview_min_side = 96.0F;
constexpr float k_preview_max_side = 360.0F;
/// GraphEditor's slot and curve constants (`GetInputSlotPos`, `DisplayLinks`).
/// A page node has one slot, so the slot sits on the vertical middle.
constexpr float k_slot_y_bias = 8.0F;
constexpr float k_curve_pull = 50.0F;

const ImU32 k_ink = IM_COL32(216, 222, 229, 255);
/// A template the graph declares but the bundle does not have.
const ImU32 k_bad = IM_COL32(224, 106, 106, 255);

/// The shell's accent and its amber, as ImGuizmo takes them (ImU32, not
/// ImVec4). The amber is the same one the status chips use for "waiting on the
/// human", which is what a popup is.
const ImU32 k_accent = IM_COL32(62, 102, 135, 255);
const ImU32 k_amber = IM_COL32(214, 178, 92, 255);

const char *k_in_names[] = {"in"};
const char *k_out_names[] = {"out"};

/// The two node kinds. A stable anchor is an edge's endpoint, so it has one
/// slot each way; a loading page and a popup are neither (load_graph rejects
/// an edge that starts or ends on one), so they have none.
constexpr std::size_t k_page_template = 0;
constexpr std::size_t k_loose_template = 1;
const GraphEditor::Template k_templates[] = {
    {k_accent, IM_COL32(26, 29, 33, 255), IM_COL32(34, 38, 44, 255), 1,
     k_in_names, nullptr, 1, k_out_names, nullptr},
    {k_amber, IM_COL32(33, 30, 25, 255), IM_COL32(42, 38, 32, 255), 0, nullptr,
     nullptr, 0, nullptr, nullptr},
};

const ImVec4 k_err_col = ImVec4(0.95F, 0.42F, 0.42F, 1.0F);

using campcat::ccat_lang::anchor_def;
using campcat::ccat_lang::anchor_template;
using campcat::ccat_lang::graph_edge;
using campcat::ccat_lang::graph_interrupt;
using campcat::ccat_lang::loaded_graph;
using campcat::ccat_lang::screen_graph;

/// One line under a node's title. `bad` paints it in the warning red. `preview`
/// is set for a template row; `open_script` is set for an interrupt row.
struct body_line {
    std::string text;
    bool bad = false;
    std::filesystem::path preview;
    std::string open_script;
};

/// A screen rectangle that hovers or double-clicks. Filled while drawing, read
/// once the canvas has finished.
struct hit_target {
    ImRect rect;
    std::filesystem::path preview;
    std::string open_script;
};

std::vector<hit_target> g_hits;
/// Where `Show` places the canvas this frame. Labels are drawn from inside it,
/// which runs after these are set.
ImVec2 g_canvas_origin;
ImVec2 g_canvas_size;
bool g_edge_labels_drawn = false;
GraphEditor::ViewState g_view;

void draw_edge_labels(ImDrawList *_dl, ImVec2 _origin, ImVec2 _canvas,
                      const GraphEditor::ViewState &_view,
                      std::vector<hit_target> *_hits);

/// A decoded template, kept between hovers. Key is the absolute path.
struct preview_tex {
    GLuint tex = 0;
    int w = 0;
    int h = 0;
    std::filesystem::file_time_type mtime{};
};
std::map<std::string, preview_tex> g_previews;

/// Whether the graph lists this anchor as a popup rather than as a place on an
/// edge.
bool is_popup(const screen_graph &_graph, std::string_view _id) {
    return std::any_of(_graph.interrupts.begin(), _graph.interrupts.end(),
                       [&_id](const graph_interrupt &_i) {
                           return _i.anchor == _id;
                       });
}

/// How many golden shots an anchor has. A missing `golden/` directory is none,
/// not an error: nothing creates it until the first shot is saved.
int count_golden(const std::filesystem::path &_dir) {
    std::error_code ec;
    int count = 0;
    if (!std::filesystem::is_directory(_dir, ec)) {
        return count;
    }
    for (const std::filesystem::directory_entry &entry :
         std::filesystem::directory_iterator(_dir, ec)) {
        if (entry.is_regular_file(ec) && entry.path().extension() == ".png") {
            ++count;
        }
    }
    return count;
}

/// The bundle name without the graph's own name in front. `shuaitu.desktop-main`
/// on the shuaitu graph reads `desktop-main`. A script that does not start
/// with that prefix is shown whole. The bundle opened on double-click is
/// always the full name.
std::string_view edge_label(std::string_view _graph, std::string_view _script) {
    if (!_graph.empty() && _script.size() > _graph.size() + 1 &&
        _script.substr(0, _graph.size()) == _graph &&
        _script[_graph.size()] == '.') {
        return _script.substr(_graph.size() + 1);
    }
    return _script;
}

/// One `must` (or `must_not`) list, a name per line: the first carries the
/// label and the rest hang under it. A name per line rather than wrapped
/// because the body font is 14px and one line holds about thirty characters,
/// which a long template name and a comma-joined list together do not fit.
void push_template_lines(std::vector<body_line> *_out,
                         const std::string &_label,
                         const std::vector<anchor_template> &_templates,
                         const std::filesystem::path &_dir) {
    const std::string pad(_label.size(), ' ');
    for (std::size_t i = 0; i < _templates.size(); ++i) {
        const anchor_template &t = _templates[i];
        std::error_code ec;
        const bool missing = !std::filesystem::is_regular_file(_dir / t.png, ec);
        body_line line;
        line.text = (i == 0 ? _label : pad) + t.png;
        line.bad = missing;
        line.preview = _dir / t.png;
        _out->push_back(std::move(line));
    }
}

/// What the node says about one anchor: how it is recognised, how it was
/// checked, and what leads into it.
std::vector<body_line> body_lines_for(const screen_graph &_graph,
                                      const anchor_def &_anchor,
                                      const std::filesystem::path &_dir) {
    std::vector<body_line> out;
    if (_anchor.id == _graph.root) {
        out.push_back({"root", false});
    }
    if (_anchor.transient) {
        out.push_back({"transient", false});
    }
    push_template_lines(&out, "must: ", _anchor.must, _dir);
    push_template_lines(&out, "must_not: ", _anchor.must_not, _dir);
    for (const graph_interrupt &interrupt : _graph.interrupts) {
        if (interrupt.anchor == _anchor.id) {
            body_line line;
            line.text = "interrupt: " +
                        std::string(edge_label(_graph.name, interrupt.script));
            line.open_script = interrupt.script;
            out.push_back(std::move(line));
        }
    }
    out.push_back({"golden: " + std::to_string(
                                  count_golden(_dir / "golden" / _anchor.id)),
                   false});
    return out;
}

/// Column pitch at zoom 1: wide enough for the longest edge label, and never
/// tighter than the old fixed gap.
float column_gap(const screen_graph &_graph) {
    ImFont *font = campcat::app::fonts_get(campcat::app::font_role::small);
    ImGui::PushFont(font);
    float widest = 0.0F;
    for (const graph_edge &edge : _graph.edges) {
        const std::string_view label = edge_label(_graph.name, edge.script);
        widest = std::max(widest, ImGui::CalcTextSize(label.data(),
                                                      label.data() + label.size())
                                       .x);
    }
    ImGui::PopFont();
    return std::max(k_gap_x, widest + 16.0F);
}

/**
 * @brief The canvas: every anchor as a node, every edge as a link, laid out
 * once and then dragged around in memory.
 *
 * Nothing here writes to the graph, which is what makes the page read-only:
 * `AllowedLink` refuses every new link and the three mutation callbacks are
 * empty, so the only change a click can make is a node's position on screen.
 */
class graph_view_delegate final : public GraphEditor::Delegate {
public:
    /// Rebuild the canvas from a graph. A graph that failed to load comes back
    /// empty, which empties the canvas.
    void reset(const loaded_graph &_loaded);

    bool AllowedLink(GraphEditor::NodeIndex, GraphEditor::NodeIndex) override {
        return false;
    }
    void SelectNode(GraphEditor::NodeIndex _i, bool _selected) override {
        m_nodes[_i].mSelected = _selected;
    }
    void MoveSelectedNodes(ImVec2 _delta) override {
        for (GraphEditor::Node &node : m_nodes) {
            if (!node.mSelected) {
                continue;
            }
            node.mRect.Min.x += _delta.x;
            node.mRect.Min.y += _delta.y;
            node.mRect.Max.x += _delta.x;
            node.mRect.Max.y += _delta.y;
        }
    }
    void AddLink(GraphEditor::NodeIndex, GraphEditor::SlotIndex,
                 GraphEditor::NodeIndex, GraphEditor::SlotIndex) override {}
    void DelLink(GraphEditor::LinkIndex) override {}
    void CustomDraw(ImDrawList *_draw_list, ImRect _rect,
                    GraphEditor::NodeIndex _i) override;
    void RightClick(GraphEditor::NodeIndex, GraphEditor::SlotIndex,
                    GraphEditor::SlotIndex) override {}

    const size_t GetTemplateCount() override {
        return sizeof(k_templates) / sizeof(k_templates[0]);
    }
    const GraphEditor::Template GetTemplate(
        GraphEditor::TemplateIndex _i) override {
        return k_templates[_i];
    }
    const size_t GetNodeCount() override { return m_nodes.size(); }
    const GraphEditor::Node GetNode(GraphEditor::NodeIndex _i) override {
        return m_nodes[_i];
    }
    const size_t GetLinkCount() override { return m_links.size(); }
    const GraphEditor::Link GetLink(GraphEditor::LinkIndex _i) override {
        return m_links[_i];
    }

private:
    /// Column = hops from root, row = order among the anchors sharing a column.
    void place(std::string_view _root, float _gap_x);
    std::optional<GraphEditor::NodeIndex> node_of(std::string_view _id) const;

    std::vector<GraphEditor::Node> m_nodes;
    std::vector<GraphEditor::Link> m_links;
    /// Holds every title the nodes point at. `Node` keeps a `const char*`, so
    /// this is filled and never touched again: growing it would dangle them.
    std::vector<std::string> m_names;
    std::vector<std::vector<body_line>> m_body;
    /// Edge targets by node index, for the layout walk.
    std::vector<std::vector<GraphEditor::NodeIndex>> m_out;
};

void graph_view_delegate::reset(const loaded_graph &_loaded) {
    const screen_graph &graph = _loaded.graph;
    m_nodes.clear();
    m_links.clear();
    m_names.clear();
    m_body.clear();
    m_out.clear();

    m_names.reserve(graph.anchors.size());
    m_body.reserve(graph.anchors.size());
    for (const anchor_def &anchor : graph.anchors) {
        m_names.push_back(anchor.id);
        m_body.push_back(body_lines_for(graph, anchor, graph.dir));
    }
    for (std::size_t i = 0; i < graph.anchors.size(); ++i) {
        const anchor_def &anchor = graph.anchors[i];
        GraphEditor::Node node;
        node.mName = m_names[i].c_str();
        node.mTemplateIndex = (anchor.transient || is_popup(graph, anchor.id))
                                  ? k_loose_template
                                  : k_page_template;
        node.mRect = ImRect(ImVec2(0.0F, 0.0F), ImVec2(k_node_w, k_node_min_h));
        m_nodes.push_back(node);
    }
    m_out.assign(graph.anchors.size(), {});

    for (const graph_edge &edge : graph.edges) {
        const std::optional<GraphEditor::NodeIndex> from = node_of(edge.from);
        const std::optional<GraphEditor::NodeIndex> to = node_of(edge.to);
        if (!from.has_value() || !to.has_value()) {
            continue; // load_graph already reported an edge naming no anchor
        }
        GraphEditor::Link link;
        // GraphEditor's field names are the reverse of what it draws:
        // DisplayLinks puts the tail of the line on mInputNodeIndex's OUTPUT
        // slot and its head on mOutputNodeIndex's INPUT slot. So `from` goes in
        // the field named input.
        link.mInputNodeIndex = *from;
        link.mInputSlotIndex = 0;
        link.mOutputNodeIndex = *to;
        link.mOutputSlotIndex = 0;
        m_links.push_back(link);
        m_out[*from].push_back(*to);
    }
    place(graph.root, column_gap(graph));
}

std::optional<GraphEditor::NodeIndex>
graph_view_delegate::node_of(std::string_view _id) const {
    for (std::size_t i = 0; i < m_names.size(); ++i) {
        if (m_names[i] == _id) {
            return i;
        }
    }
    return std::nullopt;
}

void graph_view_delegate::place(std::string_view _root, float _gap_x) {
    const std::size_t count = m_nodes.size();
    std::vector<int> rank(count, -1);
    std::vector<GraphEditor::NodeIndex> queue;
    const std::optional<GraphEditor::NodeIndex> root = node_of(_root);
    if (root.has_value()) {
        rank[*root] = 0;
        queue.push_back(*root);
    }
    for (std::size_t head = 0; head < queue.size(); ++head) {
        for (GraphEditor::NodeIndex next : m_out[queue[head]]) {
            if (rank[next] < 0) {
                rank[next] = rank[queue[head]] + 1;
                queue.push_back(next);
            }
        }
    }
    int columns = 1;
    for (const int r : rank) {
        columns = std::max(columns, r + 1);
    }
    // The column past the last hop, for everything no edge reaches: a loading
    // page and a popup both land here, since neither can be an edge's
    // endpoint.
    const std::size_t loose_col = static_cast<std::size_t>(columns);

    std::vector<float> next_y(loose_col + 1, 0.0F);
    for (std::size_t i = 0; i < count; ++i) {
        const float height =
            std::max(k_node_min_h,
                     k_header_h + k_text_pad +
                         static_cast<float>(m_body[i].size()) * k_line_h +
                         k_body_pad);
        const std::size_t col =
            rank[i] < 0 ? loose_col : static_cast<std::size_t>(rank[i]);
        const float x = static_cast<float>(col) * (k_node_w + _gap_x);
        const float y = next_y[col];
        next_y[col] = y + height + k_gap_y;
        m_nodes[i].mRect =
            ImRect(ImVec2(x, y), ImVec2(x + k_node_w, y + height));
    }
}

void graph_view_delegate::CustomDraw(ImDrawList *_draw_list, ImRect _rect,
                                     GraphEditor::NodeIndex _i) {
    _draw_list->PushClipRect(_rect.Min, _rect.Max, true);
    ImFont *font = campcat::app::fonts_get(campcat::app::font_role::small);
    const std::vector<body_line> &lines = m_body[_i];
    for (std::size_t line = 0; line < lines.size(); ++line) {
        const ImVec2 pos(_rect.Min.x + k_text_pad,
                         _rect.Min.y + k_text_pad +
                             static_cast<float>(line) * k_line_h);
        _draw_list->AddText(font, font->FontSize, pos,
                            lines[line].bad ? k_bad : k_ink,
                            lines[line].text.c_str());
        if (lines[line].preview.empty() && lines[line].open_script.empty()) {
            continue;
        }
        const float width =
            font->CalcTextSizeA(font->FontSize, FLT_MAX, 0.0F,
                                lines[line].text.c_str())
                .x;
        ImRect hit(pos, ImVec2(pos.x + width, pos.y + k_line_h));
        hit.ClipWith(_rect);
        if (hit.GetWidth() <= 0.0F || hit.GetHeight() <= 0.0F) {
            continue;
        }
        g_hits.push_back(
            {hit, lines[line].preview, lines[line].open_script});
    }
    _draw_list->PopClipRect();
    // Once per frame, on the channel GraphEditor paints after the nodes. The
    // script editor is its own window and draws later, so it covers a label
    // it overlaps instead of the label floating over the text.
    if (!g_edge_labels_drawn) {
        g_edge_labels_drawn = true;
        _draw_list->ChannelsSetCurrent(2);
        draw_edge_labels(_draw_list, g_canvas_origin, g_canvas_size, g_view,
                         &g_hits);
        _draw_list->ChannelsSetCurrent(1);
    }
}

/// The canvas's look, in the shell's palette. Built once: `Show` takes it by
/// const reference and nothing here edits it.
const GraphEditor::Options &view_options() {
    static const GraphEditor::Options options = [] {
        GraphEditor::Options o;
        o.mBackgroundColor = IM_COL32(18, 20, 23, 255);
        o.mGridColor = IM_COL32(255, 255, 255, 12);
        o.mGridColor2 = IM_COL32(255, 255, 255, 24);
        // The defaults draw a 6px border around every node, which on a 240px
        // card is a frame rather than an edge.
        o.mBorderThickness = 1.5F;
        o.mBorderSelectionThickness = 3.0F;
        o.mNodeBorderColor = IM_COL32(60, 66, 76, 255);
        o.mSelectedNodeBorderColor = IM_COL32(97, 153, 202, 255);
        o.mQuadSelection = IM_COL32(97, 153, 202, 40);
        o.mQuadSelectionBorder = IM_COL32(97, 153, 202, 200);
        o.mDefaultSlotColor = IM_COL32(150, 160, 172, 255);
        o.mFrameFocus = IM_COL32(0, 0, 0, 0); // the page has no focus to show
        o.mLineThickness = 3.0F;
        o.mGridSize = 40.0F;
        o.mMinZoom = 0.25F;
        o.mMaxZoom = 1.4F;
        return o;
    }();
    return options;
}

/// The bundles under `_home` that hold a `graph.json`, sorted (the order
/// `bundle_names` already gives).
std::vector<std::string> graph_bundles(const std::filesystem::path &_home) {
    std::vector<std::string> out;
    for (const std::string &name : campcat::ccat_lang::bundle_names(_home)) {
        std::error_code ec;
        if (std::filesystem::is_regular_file(_home / name / "graph.json", ec)) {
            out.push_back(name);
        }
    }
    return out;
}

const preview_tex *preview_for(const std::filesystem::path &_path) {
    const std::string key = _path.string();
    std::error_code ec;
    const auto stamp = std::filesystem::last_write_time(_path, ec);
    const auto found = g_previews.find(key);
    if (found != g_previews.end()) {
        const bool gone = ec && found->second.tex == 0;
        const bool same = !ec && found->second.mtime == stamp;
        if (gone || same) {
            return &found->second;
        }
        campcat::app::delete_tex(&found->second.tex, &found->second.w,
                                 &found->second.h);
    }

    preview_tex entry;
    entry.mtime = stamp;
    cv::Mat img = cv::imread(key, cv::IMREAD_UNCHANGED);
    if (img.channels() == 1) {
        cv::cvtColor(img, img, cv::COLOR_GRAY2BGR);
    }
    if (!img.empty()) {
        entry.tex = img.channels() == 4
                        ? campcat::app::upload_bgra(img, &entry.w, &entry.h,
                                                    true)
                        : campcat::app::upload_bgr(img, &entry.w, &entry.h,
                                                   true);
    }
    if (found != g_previews.end()) {
        found->second = entry;
        return &found->second;
    }
    return &g_previews.emplace(key, entry).first->second;
}

void clear_previews() {
    for (auto &pair : g_previews) {
        campcat::app::delete_tex(&pair.second.tex, &pair.second.w,
                                 &pair.second.h);
    }
    g_previews.clear();
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

void draw_template_tooltip(const std::filesystem::path &_path) {
    const preview_tex *preview = preview_for(_path);
    const std::string name = _path.filename().string();
    ImGui::BeginTooltip();
    if (preview->tex == 0) {
        ImGui::TextUnformatted("unreadable");
    } else {
        ImGui::Image(
            static_cast<ImTextureID>(static_cast<uintptr_t>(preview->tex)),
            preview_size(preview->w, preview->h), ImVec2(0.0F, 0.0F),
            ImVec2(1.0F, 1.0F));
        ImGui::Text("%s  %dx%d", name.c_str(), preview->w, preview->h);
    }
    ImGui::EndTooltip();
}

ImVec2 bezier_mid(ImVec2 _p0, ImVec2 _p1, ImVec2 _p2, ImVec2 _p3) {
    return _p0 * 0.125F + _p1 * 0.375F + _p2 * 0.375F + _p3 * 0.125F;
}

void apply_hits(const std::vector<hit_target> &_hits,
                const std::filesystem::path &_home) {
    const ImVec2 mouse = ImGui::GetMousePos();
    const bool down = ImGui::IsMouseDown(ImGuiMouseButton_Left);
    const bool dbl = ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left);
    for (const hit_target &hit : _hits) {
        if (!hit.rect.Contains(mouse)) {
            continue;
        }
        if (!down && !hit.preview.empty()) {
            draw_template_tooltip(hit.preview);
        }
        if (dbl && !hit.open_script.empty()) {
            script_editor_open(_home, hit.open_script);
        }
        return;
    }
}

std::string g_selected;
loaded_graph g_loaded;
std::string g_recover;
std::string g_error;
/// Fit the view once, on the frame after a load: `Show` resets the flag itself,
/// so it is spent the first time it is read.
bool g_need_fit = false;
graph_view_delegate g_delegate;

std::optional<GraphEditor::NodeIndex> node_index(std::string_view _id) {
    for (std::size_t i = 0; i < g_delegate.GetNodeCount(); ++i) {
        const char *name = g_delegate.GetNode(i).mName;
        if (name != nullptr && _id == name) {
            return i;
        }
    }
    return std::nullopt;
}

void draw_edge_labels(ImDrawList *_dl, ImVec2 _origin, ImVec2 _canvas,
                      const GraphEditor::ViewState &_view,
                      std::vector<hit_target> *_hits) {
    const float factor = _view.mFactor;
    const ImVec2 offset = _origin + _view.mPosition * factor;
    const ImRect canvas(_origin, _origin + _canvas);
    ImFont *font = campcat::app::fonts_get(campcat::app::font_role::small);
    const float size = font->FontSize * factor;
    for (const graph_edge &edge : g_loaded.graph.edges) {
        const std::optional<GraphEditor::NodeIndex> from = node_index(edge.from);
        const std::optional<GraphEditor::NodeIndex> to = node_index(edge.to);
        if (!from.has_value() || !to.has_value()) {
            continue;
        }
        const GraphEditor::Node a = g_delegate.GetNode(*from);
        const GraphEditor::Node b = g_delegate.GetNode(*to);
        const ImVec2 size_a = a.mRect.GetSize() * factor;
        const ImVec2 size_b = b.mRect.GetSize() * factor;
        const ImVec2 p_from(offset.x + a.mRect.Max.x * factor,
                            offset.y + a.mRect.Min.y * factor +
                                size_a.y * 0.5F + k_slot_y_bias);
        const ImVec2 p_to(offset.x + b.mRect.Min.x * factor,
                          offset.y + b.mRect.Min.y * factor + size_b.y * 0.5F +
                              k_slot_y_bias);
        const ImVec2 mid =
            bezier_mid(p_from, p_from + ImVec2(k_curve_pull, 0.0F) * factor,
                       p_to + ImVec2(-k_curve_pull, 0.0F) * factor, p_to);
        const std::string_view label =
            edge_label(g_loaded.graph.name, edge.script);
        const ImVec2 text = font->CalcTextSizeA(size, FLT_MAX, 0.0F,
                                                 label.data(),
                                                 label.data() + label.size());
        const ImVec2 pad(4.0F * factor, 2.0F * factor);
        const ImRect box(mid - text * 0.5F - pad, mid + text * 0.5F + pad);
        ImRect visible = box;
        visible.ClipWith(canvas);
        if (visible.GetWidth() <= 0.0F || visible.GetHeight() <= 0.0F) {
            continue;
        }
        _dl->AddRectFilled(box.Min, box.Max, view_options().mBackgroundColor);
        _dl->AddText(font, size, mid - text * 0.5F, k_ink, label.data(),
                     label.data() + label.size());
        _hits->push_back({visible, {}, edge.script});
    }
}

/// Read `_name` and rebuild the canvas.
///
/// Called only when the selection changes or the user asks for it: `load_graph`
/// stats every template and parses every edge script, which is not something to
/// repeat every frame.
void load_graph(const std::filesystem::path &_home, const std::string &_name) {
    clear_previews();
    g_loaded = campcat::ccat_lang::load_graph(_home, _name);
    // A failed load comes back with the graph cleared, so this leaves the
    // canvas empty rather than showing the graph that was there before.
    g_delegate.reset(g_loaded);
    g_error = g_loaded.ok ? std::string() : g_loaded.error;
    g_recover.clear();
    if (!g_loaded.ok) {
        return;
    }
    g_recover = g_loaded.graph.recover;
    g_need_fit = true;
    g_view = GraphEditor::ViewState{};
}

} // namespace

void graph_panel_draw(const campcat::app_config &_cfg_view) {
    script_editor_draw_windows();
    const std::filesystem::path home = _cfg_view.config_home;
    const std::vector<std::string> names = graph_bundles(home);
    if (names.empty()) {
        ImGui::TextDisabled("No graph under %s", home.string().c_str());
        return;
    }

    // The selected graph can go away under the page -- deleted, or renamed by a
    // tool -- and the canvas must go with it rather than point at a file that
    // is no longer there.
    if (!g_selected.empty() &&
        std::find(names.begin(), names.end(), g_selected) == names.end()) {
        g_selected.clear();
        g_loaded = loaded_graph{};
        g_recover.clear();
        g_error.clear();
        g_delegate.reset(g_loaded);
        clear_previews();
    }

    const ImGuiStyle &style = ImGui::GetStyle();
    const float reload_w =
        ImGui::CalcTextSize("Reload").x + style.FramePadding.x * 2.0F;
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - reload_w -
                            style.ItemSpacing.x);
    if (ImGui::BeginCombo("##graph_bundle",
                          g_selected.empty() ? "(no graph)"
                                             : g_selected.c_str())) {
        for (const std::string &name : names) {
            if (ImGui::Selectable(name.c_str(), name == g_selected) &&
                name != g_selected) {
                g_selected = name;
                load_graph(home, name);
            }
        }
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    if (g_selected.empty()) {
        ImGui::BeginDisabled();
    }
    const bool reload_clicked = ImGui::Button("Reload");
    if (g_selected.empty()) {
        ImGui::EndDisabled();
    }
    if (reload_clicked && !g_selected.empty()) {
        load_graph(home, g_selected);
    }

    if (g_selected.empty()) {
        ImGui::TextDisabled("Pick a graph.");
        return;
    }
    if (!g_error.empty()) {
        ImGui::TextColored(k_err_col, "%s", g_error.c_str());
        return;
    }
    // `recover` is a bundle rather than a place, so it is not a node.
    ImGui::TextDisabled("recover: %s",
                        g_recover.empty() ? "(none)" : g_recover.c_str());

    GraphEditor::FitOnScreen fit =
        g_need_fit ? GraphEditor::Fit_AllNodes : GraphEditor::Fit_None;
    g_need_fit = false;
    g_hits.clear();
    g_edge_labels_drawn = false;
    g_canvas_origin = ImGui::GetCursorScreenPos();
    g_canvas_size = ImGui::GetContentRegionAvail();
    // Always enabled: with it false `Show` draws the grid and nothing else,
    // and read-only is enforced by the delegate refusing every change.
    // Edge labels are submitted from CustomDraw, on the canvas child.
    // `Show` zooms whenever the cursor is inside the canvas rectangle, which
    // is also true when the script editor covers that spot and is the window
    // the wheel already scrolled. Drop the wheel unless this page is the one
    // under the cursor, and put it back for whatever is drawn after.
    ImGuiIO &io = ImGui::GetIO();
    const float wheel = io.MouseWheel;
    if (!ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows)) {
        io.MouseWheel = 0.0F;
    }
    GraphEditor::Show(g_delegate, view_options(), g_view, true, &fit);
    io.MouseWheel = wheel;
    apply_hits(g_hits, home);
}
