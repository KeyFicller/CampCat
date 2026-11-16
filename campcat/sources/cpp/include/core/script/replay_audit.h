#pragma once

#include <filesystem>
#include <fstream>
#include <map>
#include <opencv2/core.hpp>
#include <optional>
#include <string>
#include <vector>

namespace campcat::ccat_lang {

struct replay_match {
    std::string path;
    double score = 0;
    double threshold = 0;
    bool found = false;
    bool on_after_frame = false;
    std::string fingerprint;
};

struct replay_step {
    unsigned long long id = 0;
    unsigned long long source_id = 0;
    std::string script, action;
    int line = 0, column = 0;
    bool ok = false, value = true;
    std::string error_code, message;
    double elapsed_ms = 0;
    cv::Mat before, after;
    std::vector<replay_match> matches;
    std::vector<std::string> operands;
    std::vector<double> coordinates;
    std::vector<int> input_pixels;
    std::optional<int> duration_ms;
    bool compare = false;
};

/// One deterministic replay's evidence. I/O failures throw: the interpreter
/// stops instead of silently continuing a run whose evidence could not be
/// saved.
class replay_audit {
public:
    replay_audit(const std::filesystem::path &_root, unsigned _keep_runs,
                 unsigned long long _max_bytes);
    void start(const replay_step &_step);
    void record(const replay_step &_step);
    unsigned long long source(const std::string &_script,
                              const std::string &_source);
    void finish(bool _ok, const std::string &_code,
                const std::string &_message);
    const std::filesystem::path &path() const { return m_dir; }

private:
    std::optional<std::string> save(const cv::Mat &_frame,
                                    const std::string &_name);
    void append(const std::string &_line, bool _terminal = false);
    void reserve(unsigned long long _bytes) const;
    std::filesystem::path m_dir;
    std::ofstream m_log;
    unsigned long long m_bytes = 0, m_limit;
    unsigned m_keep_runs;
    unsigned m_terminal_bytes = 0;
    bool m_record_failed = false;
    std::map<std::pair<std::string, std::string>, unsigned long long> m_sources;
};
} // namespace campcat::ccat_lang
