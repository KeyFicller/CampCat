#pragma once

#include "core/script/ccat_ast.h"

#include <filesystem>
#include <functional>
#include <string>

namespace campcat {

class adb_api;
class llm_host;

/**
 * @brief Build the `ask()` answerer for a GUI script run.
 *
 * The local model answers, not the human: the script's question goes to the
 * sidecar with a fresh screenshot, and neither a dialog nor the conversation
 * is involved. A question the model will not answer comes back with an error,
 * so the script stops instead of taking a branch nobody chose.
 *
 * @param[in] _adb Takes the frame at the moment of the question; the run owns
 * the device, so this is the same screen the script just acted on.
 * @param[in] _host The process-wide host (`consult_host`), so the run reaches
 * the sidecar the conversation uses.
 * @param[in] _repo_root Passed to `llm_host::consult`, which finds the sidecar
 * under it and starts it if the Run button came first.
 * @param[in] _provider The provider the settings selected; a consult builds its
 * own model, so it has to be told.
 * @param[in] _should_stop Polled while waiting, so Stop is not held up by a
 * slow model.
 *
 * Any of the first three being empty yields a handler that reports nobody can
 * answer, which is what a script with an `ask` then stops on.
 */
ccat_lang::ask_handler
make_consult_ask(adb_api *_adb, llm_host *_host,
                 std::filesystem::path _repo_root, std::string _provider,
                 std::function<bool()> _should_stop);

} // namespace campcat
