#include "core/script/replay_audit.h"
#include "core/automation_log.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <nlohmann/json.hpp>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>
#include <stdexcept>

namespace campcat::ccat_lang {
namespace {
using nlohmann::json;
std::atomic<unsigned long long> sequence{0};

json difference(const replay_step &_step) {
    if (!_step.compare || _step.before.empty() || _step.after.empty())
        return {{"status", "unavailable"}, {"changed_ratio", nullptr}};
    if (_step.before.size() != _step.after.size() ||
        _step.before.type() != _step.after.type())
        return {{"status", "incomparable"}, {"changed_ratio", nullptr}};
    cv::Mat delta, gray;
    cv::absdiff(_step.before, _step.after, delta);
    cv::cvtColor(delta, gray, cv::COLOR_BGR2GRAY);
    // Ignore small pixel noise; this is an observation, never a success
    // verdict.
    const double ratio =
        static_cast<double>(cv::countNonZero(gray > 12)) / gray.total();
    return {{"status", ratio > .001 ? "changed" : "no_change"},
            {"changed_ratio", ratio}};
}
} // namespace

replay_audit::replay_audit(const std::filesystem::path &_root,
                           unsigned _keep_runs, unsigned long long _max_bytes)
    : m_limit(_max_bytes), m_keep_runs(std::max(1u, _keep_runs)) {
    std::filesystem::create_directories(_root);
    const auto ns = std::chrono::system_clock::now().time_since_epoch().count();
    do {
        m_dir = _root / ("run-" + std::to_string(ns) + "-" +
                         std::to_string(++sequence));
    } while (!std::filesystem::create_directory(m_dir));
    m_log.open(m_dir / "actions.jsonl", std::ios::binary | std::ios::app);
    if (!m_log)
        throw std::runtime_error("cannot open replay actions.jsonl");
    append(json{{"event", "run_start"},
                {"schema_version", 1},
                {"run_id", m_dir.filename().string()}}
               .dump());
}

void replay_audit::reserve(unsigned long long _bytes) const {
    if (_bytes > m_limit || m_bytes > m_limit - _bytes)
        throw std::runtime_error("replay audit size limit reached");
}

void replay_audit::append(const std::string &_line, bool _terminal) {
    // Leave room to explain a size-budget stop. Only a compact error receipt
    // and run_end may use this allowance, at most 4 KiB each.
    if (!_terminal)
        reserve(_line.size() + 1);
    else if (_line.size() > 4095 || m_terminal_bytes + _line.size() + 1 > 8192)
        throw std::runtime_error("replay error receipt too large");
    m_log << _line << '\n';
    m_log.flush();
    if (!m_log)
        throw std::runtime_error("cannot write replay actions.jsonl");
    m_bytes += _line.size() + 1;
    if (_terminal)
        m_terminal_bytes += _line.size() + 1;
}

void replay_audit::start(const replay_step &_step) {
    if (m_record_failed)
        throw std::runtime_error("replay evidence storage failed");
    append(json{{"event", "step_start"},
                {"step_id", _step.id},
                {"source_id", _step.source_id},
                {"script", _step.script},
                {"line", _step.line},
                {"column", _step.column},
                {"action", _step.action},
                {"operands", _step.operands},
                {"coordinates", _step.coordinates}}
               .dump());
}

std::optional<std::string> replay_audit::save(const cv::Mat &_frame,
                                              const std::string &_name) {
    if (_frame.empty())
        return std::nullopt;
    std::vector<unsigned char> png;
    if (!cv::imencode(".png", _frame, png))
        throw std::runtime_error("cannot encode replay frame");
    reserve(png.size());
    const auto file = m_dir / (_name + ".png");
    std::ofstream out(file, std::ios::binary);
    out.write(reinterpret_cast<const char *>(png.data()), png.size());
    out.close();
    if (!out)
        throw std::runtime_error("cannot save replay frame: " + file.string());
    m_bytes += png.size();
    return file.string();
}

void replay_audit::record(const replay_step &_step) {
    // A child already recorded the storage failure. Do not spend the terminal
    // allowance again as each enclosing run() unwinds with the same error.
    if (m_record_failed)
        return;
    const std::string id = std::to_string(_step.id);
    std::optional<std::string> before, after;
    try {
        before = save(_step.before, id + "-before");
        after = save(_step.after, id + "-after");
        json matches = json::array();
        for (const auto &m : _step.matches) {
            const auto &shot = m.on_after_frame ? after : before;
            matches.push_back({{"template", m.path},
                               {"score", m.score},
                               {"threshold", m.threshold},
                               {"found", m.found},
                               {"fingerprint", m.fingerprint},
                               {"shot", shot ? json(*shot) : json(nullptr)}});
        }
        append(json{{"event", "step"},
                    {"step_id", _step.id},
                    {"source_id", _step.source_id},
                    {"script", _step.script},
                    {"line", _step.line},
                    {"column", _step.column},
                    {"action", _step.action},
                    {"ok", _step.ok},
                    {"value", _step.value},
                    {"error_code", _step.error_code},
                    {"message", _step.message},
                    {"elapsed_ms", _step.elapsed_ms},
                    {"matches", matches},
                    {"operands", _step.operands},
                    {"coordinates", _step.coordinates},
                    {"input_pixels", _step.input_pixels},
                    {"duration_ms", _step.duration_ms ? json(*_step.duration_ms)
                                                      : json(nullptr)},
                    {"before_shot", before ? json(*before) : json(nullptr)},
                    {"after_shot", after ? json(*after) : json(nullptr)},
                    {"diff", difference(_step)}}
                   .dump());
    } catch (const std::exception &ex) {
        m_record_failed = true;
        append(json{{"event", "step"},
                    {"step_id", _step.id},
                    {"source_id", _step.source_id},
                    {"script", _step.script.substr(0, 1024)},
                    {"line", _step.line},
                    {"column", _step.column},
                    {"action", _step.action},
                    {"ok", false},
                    {"error_code", "storage_error"},
                    {"message", std::string(ex.what()).substr(0, 512)},
                    {"before_shot", before ? json(*before) : json(nullptr)},
                    {"after_shot", after ? json(*after) : json(nullptr)},
                    {"diff",
                     {{"status", "unavailable"}, {"changed_ratio", nullptr}}}}
                   .dump(),
               true);
        throw;
    }
}

unsigned long long replay_audit::source(const std::string &_script,
                                        const std::string &_source) {
    const auto key = std::make_pair(_script, _source);
    if (const auto it = m_sources.find(key); it != m_sources.end())
        return it->second;
    const auto id = m_sources.size() + 1;
    append(json{{"event", "source"},
                {"source_id", id},
                {"script", _script},
                {"source", _source}}
               .dump());
    m_sources.emplace(key, id);
    return id;
}

void replay_audit::finish(bool _ok, const std::string &_code,
                          const std::string &_message) {
    append(json{{"event", "run_end"},
                {"ok", _ok},
                {"error_code", _code},
                {"message", _message.substr(0, 2048)}}
               .dump(),
           true);
    std::ofstream marker(m_dir / "complete");
    marker.close();
    if (!marker)
        throw std::runtime_error("cannot mark replay audit complete");
    // Delete previous evidence only after recording the replacement run's end.
    // An incomplete or failed-to-open run is never a replacement.
    try {
        std::vector<std::filesystem::path> completed;
        for (const auto &entry :
             std::filesystem::directory_iterator(m_dir.parent_path())) {
            if (entry.is_directory() &&
                entry.path().filename().string().starts_with("run-") &&
                std::filesystem::is_regular_file(entry.path() / "complete"))
                completed.push_back(entry.path());
        }
        std::sort(completed.begin(), completed.end());
        const auto keep = m_keep_runs;
        while (completed.size() > keep) {
            std::filesystem::remove_all(completed.front());
            completed.erase(completed.begin());
        }
    } catch (const std::exception &ex) {
        automation_log::emit("[ccat] replay retention failed: " +
                             std::string(ex.what()));
    }
}
} // namespace campcat::ccat_lang
