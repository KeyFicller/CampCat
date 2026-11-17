#pragma once

#include "app/ui/activity_status.h"

#include <imgui.h>

namespace campcat::app {

/// Optional parts of a card. Designated initializers must follow this order.
struct card_options {
    const char *hint = nullptr;              ///< one text_2 line under the title
    const char *glyph = nullptr;             ///< icon before the title
    bool collapsible = true;
    float width = 0.0F;                      ///< 0 fills the row
    const activity_status *status = nullptr; ///< pill at the header's right end, or under the title when it does not fit
    ImVec4 bar{};                            ///< w > 0: 3px bar on the left edge
};

/**
 * @brief Open a card: a bordered `surface_2` box with a bold title row.
 *
 * A collapsible card keeps its open flag in the parent window's storage, keyed
 * by `_id` and open by default, so it survives page switches but not restarts
 * -- the same lifetime `CollapsingHeader` had.
 *
 * @return true when the body should be drawn. Call `card_end` either way.
 */
bool card_begin(const char *_id, const char *_title, const card_options &_opt = {});

/// @brief Close the card `card_begin` opened, open or not.
void card_end();

/// @brief Width `pill` takes in the current font.
float pill_width(const activity_status &_s);

/// @brief A dot and a word in the level's colour, at the cursor.
void pill(const activity_status &_s);

/// @brief Small text on a 14% wash of `_color`, fully rounded, vertically
/// centred on the current frame height.
void tag(const char *_text, const ImVec4 &_color);

/**
 * @brief A row of 2-3 mutually exclusive options in one frame. Drops to the
 * next line when it does not fit beside the item before it.
 * @return true only when `*_index` changed; re-clicking the active option is
 * not a change.
 */
bool segmented(const char *_id, const char *const *_labels, int _count, int *_index);

/// @brief A text_2 label in a fixed column; the next widget fills the rest of
/// the row.
void form_label(const char *_label, float _label_w = 160.0F);

/// @brief Repaint an item's edge in the accent while `_active`.
void focus_ring(const ImVec2 &_min, const ImVec2 &_max, float _rounding, bool _active);

/// @brief `min(available width, _max)`.
float content_width(float _max);

/// @brief `SameLine()` while the next item, `_w` wide, still fits after the
/// last one; otherwise it starts the next row. Docked columns can be too narrow
/// for a whole button row.
void same_line_if_fits(float _w);

/// @brief An icon tile, a display-size title and a text_2 hint, centred
/// horizontally with the block's middle 40% down the available height.
void empty_state(const char *_glyph, const char *_title, const char *_hint);

} // namespace campcat::app
