#pragma once

namespace campcat {
struct app_config;
}

/**
 * @brief Draw the Graph page: pick a bundle that holds a `graph.json`, then
 * draw its anchors and edges as a node graph.
 *
 * Read-only. The canvas pans, zooms, selects and drags nodes, but nothing it
 * does reaches the disk: a new link cannot be drawn and no node moves on the
 * next load. A `graph.json` that will not load is reported in place of the
 * canvas.
 *
 * @param[in] _cfg_view This frame's config view, read for `config_home`.
 */
void graph_panel_draw(const campcat::app_config &_cfg_view);
