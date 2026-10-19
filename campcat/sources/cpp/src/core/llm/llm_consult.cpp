#include "core/llm/llm_consult.h"

#include "core/adb_client.h"
#include "core/llm/llm_host.h"
#include "core/llm/llm_protocol.h"

#include <opencv2/imgcodecs.hpp>

#include <utility>
#include <vector>

namespace campcat {

namespace {

constexpr int k_screencap_timeout_ms = 45000;

/// The question as the model needs it: the screen it is about, base64 as it
/// travels. The run owns adb, so this is a plain capture rather than a tool --
/// `adb_busy` would otherwise refuse the very script that is asking.
ccat_lang::ask_result ask_once(adb_api &_adb, llm_host &_host,
                               const std::filesystem::path &_repo_root,
                               const std::string &_provider,
                               std::string_view _question,
                               const std::function<bool()> &_should_stop) {
    if (_should_stop && _should_stop()) {
        return ccat_lang::ask_result{std::nullopt, "ask: stopped"};
    }
    cv::Mat bgr;
    std::string diagnostic;
    if (!_adb.screencap_bgr(&bgr, k_screencap_timeout_ms, &diagnostic)) {
        return ccat_lang::ask_result{std::nullopt, "ask: screencap failed"};
    }
    std::vector<unsigned char> png;
    if (!cv::imencode(".png", bgr, png)) {
        return ccat_lang::ask_result{std::nullopt, "ask: screencap failed"};
    }
    const std::string image = llm_protocol::base64_encode(png.data(),
                                                          png.size());
    return _host.consult(_repo_root, _provider, _question, image,
                         _should_stop);
}

} // namespace

ccat_lang::ask_handler
make_consult_ask(adb_api *_adb, llm_host *_host,
                 std::filesystem::path _repo_root, std::string _provider,
                 std::function<bool()> _should_stop) {
    if (_adb == nullptr || _host == nullptr) {
        return {};
    }
    return [_adb, _host, repo_root = std::move(_repo_root),
            provider = std::move(_provider),
            should_stop = std::move(_should_stop)](
               std::string_view _question) {
        return ask_once(*_adb, *_host, repo_root, provider, _question,
                        should_stop);
    };
}

} // namespace campcat
