#pragma once

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace campcat {

/**
 * @brief The prompts of one smoke run, out of the list file's text.
 *
 * One instruction per line. Blank lines and `#` comments are skipped, the
 * spaces and tabs around a line are dropped, and a Windows CR before the
 * newline goes with them -- that CR would otherwise reach the model.
 *
 * @param[in] _text Whole file contents.
 * @return The instructions in file order; empty when the file holds none.
 */
std::vector<std::string> smoke_prompts(std::string_view _text);

/**
 * @brief Build one graph on the demo app and print what it took.
 *
 * Boots the emulator, connects adb, then runs every prompt of `_prompts`
 * through the same sidecar, prompts and tools the conversation page uses. The
 * graph lands in a fresh `smoke/<timestamp>/` directory, which also becomes the
 * turn's `config_home`: the interpreter reads anchors from that field, so the
 * real library's screens never leak into the run.
 *
 * Timing starts at the first line of this function and covers everything up to
 * the last turn: boot, adb, sidecar start-up, every model call. It is printed
 * even when the run fails, because a failed run's cost is still a measurement.
 *
 * @param[in] _prompts List file (see `smoke_prompts`).
 * @param[in] _config `config.json`; its `config_home` names the real library,
 * copied from for `CCAT.md` and the notes, and used for the checkout root.
 * @return 0 when every turn succeeded, 1 otherwise.
 */
int run_smoke(const std::filesystem::path &_prompts,
              const std::filesystem::path &_config);

} // namespace campcat
