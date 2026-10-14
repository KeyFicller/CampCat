#pragma once

#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "core/app_config.h"
#include "core/mumu_launcher.h"

namespace campcat {
class adb_api;
} // namespace campcat

namespace campcat::llm {

/// A rectangle on the screen, in screenshot pixels. A plain aggregate on
/// purpose: this is the internal currency -- approval, drawing, geometry --
/// while the model-facing spelling of the same shape is a private struct in
/// `llm_tools.cpp`, where the schema annotations live (a public header cannot
/// include the reflective one).
///
/// No default member initializers: an absent key must be an error rather than a
/// silent zero, and a defaulted pair of shapes would decode into each other.
struct box {
    int x; ///< Left edge X, screenshot px
    int y; ///< Top edge Y
    int w; ///< Width
    int h; ///< Height

    /// Equality, so a human's redraw can be compared with the model's region
    /// without spelling the four fields out at each comparison.
    friend bool operator==(const box &, const box &) = default;
};

/// A filled disc: centre (cx,cy) and radius r, screenshot px. Covers the pixels
/// where `dx*dx + dy*dy <= r*r`, so its bounding box is `2r+1` square.
struct circle {
    int cx; ///< Centre X, screenshot px
    int cy; ///< Centre Y
    int r;  ///< Radius, at least 1

    friend bool operator==(const circle &, const circle &) = default;
};

/// Either shape: what the approval layer, the correction UI and the geometry
/// helpers speak. Adding a shape means adding an alternative here, plus a branch
/// wherever the shapes actually differ (bounds, crop, draw).
using region = std::variant<box, circle>;

/// One question for the human, asked before a gated tool is allowed to run.
struct approval_request {
    std::string tool;               ///< Gated tool name, e.g. "tap"
    std::string summary;            ///< One line for the human
    std::vector<region> highlight;  ///< Regions to draw on the screenshot
    std::string screen_png; ///< Raw PNG bytes of the latest screenshot (not
                            ///< base64); may be empty
    /// Whether the human may redraw this region (tap / save_template_box set it).
    /// The UI keys on this flag, so it never has to know tool names.
    bool editable = false;
    /// The region the model gave, set only when it is not the one being acted on
    /// (the optional CV snap moved it). The UI draws this one thin and dim, so
    /// the human sees both what was asked for and what will actually happen.
    std::optional<region> given_region;
};

/// How a human answered an approval_request. `guidance` and `redrawn` are both
/// corrections, so they only mean anything when `approved` is false.
struct approval_decision {
    bool approved = false;
    std::string guidance;
    /// The region the human redrew; same rule as the guidance (an approval means
    /// the region was right, so this is dropped).
    std::optional<region> redrawn;
};

/// The screenshot the model is working from. Raw PNG bytes (not base64): the
/// wire encoding happens once, on the way out. Both fields live and die
/// together, and outlive the turn they were taken in (see `current_screen`).
struct screen_state {
    int w = 0;
    int h = 0;
    std::string png;
};

/**
 * @brief Runtime state the tools read but the model must never see.
 *
 * The adb session is deliberately absent from every tool signature: it is
 * environment, not an argument. The screenshot under discussion is the same
 * kind of thing, and it outlives the turn: `llm_host` keeps it (see
 * `current_screen`) and injects it here each turn, so a tap at coordinates
 * taken from it stays valid until the model asks for a fresh one.
 */
struct tool_context {
    adb_api *adb = nullptr;
    /// True while an automation cycle owns adb, so tools must refuse rather
    /// than compete for the device. An empty function means "never busy".
    std::function<bool()> adb_busy;
    /// Last screenshot of the session; injected by `llm_host::run_turn`.
    screen_state screen;
    /// Gated tools ask the human through this. An empty function means there is
    /// nobody to ask, and the tool then refuses: a gate that silently opens is
    /// worse than no gate.
    std::function<approval_decision(const approval_request &)> request_approval;
    /// Mirrors app_config's approval settings, folded into one answer about the
    /// tool asking: whether that tool shows the human what it is about to do
    /// and waits for approval. The shell supplies it (`llm_panel`), so a tool
    /// reads one answer about its own name instead of a flag plus a table of
    /// its own. Empty means never: a context built without one gates nothing,
    /// which is what the default means.
    std::function<bool(std::string_view _tool)> needs_approval;
    /// The snap settings for this call. Read live, like `needs_approval`: the
    /// LLM popup's snap switch takes effect on the next tool call rather than
    /// the next turn, so this is a getter rather than the copy in `cfg`. Empty
    /// falls back to a default-constructed `box_snap_config`, which snaps
    /// nothing.
    std::function<box_snap_config()> box_snap;
    /// Config the script tools hand to the interpreter (matcher thresholds,
    /// pacing). Null makes `run_script` refuse rather than run with defaults
    /// nobody chose.
    const app_config *cfg = nullptr;
    /// Root a model-authored script's relative paths (template PNGs, `run`)
    /// resolve against. It has no file of its own, so this stands in for its
    /// directory.
    std::filesystem::path script_dir;
    /// Starts the emulator this shell is configured for and waits until it is
    /// up. Supplied by the shell: the launch, its watchdog and the shutdown
    /// bookkeeping all live there. Runs on the tool thread and blocks for as
    /// long as the boot takes. `_should_stop` is polled by the wait: true means
    /// give up early and report it rather than that something broke. An empty
    /// function means nobody can start one, and `start_emulator` then refuses
    /// -- silence would look like success.
    std::function<campcat::mumu_status(
        const std::function<bool()> &_should_stop)>
        start_emulator;
};

/// Outcome of one tool call. `image_png` is set only by image-returning tools
/// and holds raw PNG bytes; the host base64-encodes them for the wire, which is
/// the only place the encoding exists.
struct tool_reply {
    bool ok = false;
    std::string text;
    std::string error;
    std::string image_png;
    /// Line the UI's tool note shows instead of `text`; non-empty also marks
    /// the text as a document for the model, too long to print (see
    /// `js::quiet`).
    /// `{param}` in it is filled from this call's arguments, so the log can say
    /// which script was read rather than only that one was.
    std::string quiet_note;
    /// The session number this call filed a frame under. Only `screenshot` sets
    /// it; it is what the model's `shot` arguments name later, so the host puts
    /// it on the wire and the sidecar puts it in the caption below the picture.
    int shot = 0;
};

/// Sets the context every tool reads. Called once per turn by the worker that
/// runs the sidecar exchange; that same thread performs every dispatch, so no
/// lock is needed. Passing a default-constructed context disables the tools.
void set_context(const tool_context &_ctx);

/// Whether `_gates` makes `_tool` ask the human before acting: the master
/// switch folded with that tool's own gate. The names are the ones the tools
/// report in their approval requests, so a tool asks about itself and there is
/// no second table to keep in step. A gated name the table has not heard of
/// answers true rather than false: an unguarded surprise is the one failure
/// worth avoiding here.
bool approval_gate_required(const approval_config &_gates,
                            std::string_view _tool);

/// The screenshot the tools currently hold: the one injected this turn, or the
/// one `screenshot` replaced it with. `run_turn` takes it back at the end of a
/// turn and injects it again next turn: the screenshot belongs to the session,
/// not to the turn.
void current_screen(screen_state *_out);

/// The OpenAI tools array, generated by reflection from the tool declarations.
std::string tools_json();

/// Runs one tool by name against the model's raw `arguments` JSON text.
/// Never throws: every failure comes back as `ok == false`.
tool_reply dispatch(std::string_view _name, std::string_view _args_json);

/// True while a script started by `run_script` is executing. The UI polls it to
/// decide whether its stop control is worth showing.
bool script_running();

/// Asks the script that is running to stop before its next statement. Safe to
/// call when nothing is running.
void request_script_stop();

/// True while `start_emulator` is waiting for the emulator. The UI polls it to
/// decide whether its Stop control is worth showing.
bool emulator_starting();

/// Asks the emulator wait to give up before its next poll. Safe to call when
/// nothing is waiting.
void request_emulator_stop();

/// Drops every numbered screenshot this session kept, so `shot` numbering
/// starts over. Called when the session ends, in step with clearing the current
/// screen: a frame from a finished conversation must not answer a later lookup.
void forget_frames();

} // namespace campcat::llm
