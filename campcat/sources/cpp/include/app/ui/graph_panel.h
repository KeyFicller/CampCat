#pragma once

namespace campcat {
struct app_config;
}

/**
 * @brief Draw the Graph page: pick a graph (a `<G>.entry` anchor bundle), then
 * draw its anchors and edges as a node graph.
 *
 * Read-only. The canvas pans, zooms, selects and drags nodes, but nothing it
 * does reaches the disk: a new link cannot be drawn and no node moves on the
 * next load. Double-clicking an edge's script name, an anchor's kind line, or a
 * popup's close line opens that bundle in the script editor. The graph's
 * problems are listed above the canvas.
 *
 * @param[in] _cfg_view This frame's config view, read for `config_home`.
 */
void graph_panel_draw(const campcat::app_config &_cfg_view);
