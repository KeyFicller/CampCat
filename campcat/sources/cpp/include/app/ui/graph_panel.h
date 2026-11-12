#pragma once

namespace campcat {
struct app_config;
struct workspace_selection;
}

/**
 * @brief Draw the workspace toolbar's selected graph as anchors and edges.
 *
 * Read-only. The canvas pans, zooms, selects and drags nodes, but nothing it
 * does reaches the disk: a new link cannot be drawn and no node moves on the
 * next load. Double-clicking an edge's script name, an anchor's kind line, or a
 * popup's close line opens that bundle in the script editor. The graph's
 * problems are listed above the canvas.
 *
 * @param[in] _cfg_view This frame's config view, read for `config_home`.
 */
void graph_panel_draw(const campcat::app_config &_cfg_view,
                       campcat::workspace_selection &_selection);

/// Focus the workspace's skill when entering the graph through Show graph.
void graph_panel_focus_selection();
