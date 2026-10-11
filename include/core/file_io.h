#pragma once

#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>

namespace campcat {

/// Writes `_text` to a temp sibling and renames it over `_path`, so a crash or a
/// full disk leaves the old file rather than a half-written one. The temp name is
/// fixed, not unique: a leftover from a crash is truncated by the next write.
/// @throw std::runtime_error naming the step and path that failed.
inline void write_file_atomic(const std::filesystem::path &_path,
                              std::string_view _text) {
  const auto fail = [&](const std::string &_why) -> void {
    throw std::runtime_error(_why);
  };

  std::error_code ec;
  if (!_path.parent_path().empty()) {
    std::filesystem::create_directories(_path.parent_path(), ec);
    if (ec) {
      fail("could not create " + _path.parent_path().string() + ": " +
           ec.message());
    }
  }

  const std::filesystem::path tmp = _path.string() + ".tmp";
  {
    std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
    if (!out) {
      fail("could not open " + tmp.string() + " for write");
    }
    out << _text;
    out.flush();
    if (!out) {
      std::error_code ignored;
      std::filesystem::remove(tmp, ignored);
      fail("could not write " + tmp.string());
    }
  }

  // The rename is what makes this atomic: a reader sees the old file or the new
  // one, never a partial write.
  std::filesystem::rename(tmp, _path, ec);
  if (ec) {
    std::error_code ignored;
    std::filesystem::remove(tmp, ignored);
    fail("could not replace " + _path.string() + ": " + ec.message());
  }
}

} // namespace campcat
