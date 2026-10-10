#pragma once

#include <cstddef>
#include <filesystem>
#include <string>

namespace campcat::llm {

/// Upper bound on the memory file, roughly 800 tokens. A write past this is
/// refused rather than truncated: notes that are too long are a summarizer that
/// ignored its budget, and the old notes are merely stale.
/// Mirrored by `DEFAULT_MEMORY_MAX_CHARS` in `llm/models.py`. A constant cannot
/// be shared across the two languages, so keep them equal by hand -- a mismatch
/// shows up as memory that never updates, because the sidecar writes what the
/// host then refuses.
constexpr std::size_t k_memory_max_chars = 4000;

/// `<config_home>/llm/memory.md`.
std::filesystem::path memory_path(const std::filesystem::path &_config_home);

/// Read the notes into `_out`.
///
/// A missing file is an empty memory, not an error. Returns false only when the
/// file exists and cannot be read, which is what stops `write_memory` from
/// overwriting something it could not see.
bool read_memory(const std::filesystem::path &_config_home, std::string *_out);

/// Replace the notes, atomically: a temp file in the same directory, then a
/// rename, so a process killed mid-write leaves the old file intact instead of
/// half of the new one. Refused, with the old file untouched, when the text is
/// empty, is over `k_memory_max_chars`, or the current file cannot be read.
bool write_memory(const std::filesystem::path &_config_home,
                  const std::string &_text);

/// Delete the memory file. Already absent counts as success.
bool clear_memory(const std::filesystem::path &_config_home);

} // namespace campcat::llm
