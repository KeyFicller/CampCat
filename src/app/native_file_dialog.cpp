#include "app/native_file_dialog.h"

#include <portable-file-dialogs.h>

#include <exception>
#include <vector>

namespace campcat {

std::optional<std::filesystem::path>
pick_open_file(const std::filesystem::path &_start_dir,
               const std::string &_extension, const std::string &_title) {
  std::string ext = _extension;
  if (!ext.empty() && ext.front() == '.') {
    ext.erase(ext.begin());
  }

  std::vector<std::string> filters;
  if (!ext.empty()) {
    filters.push_back(ext + " files");
    filters.push_back("*." + ext);
  }
  filters.push_back("All files");
  filters.push_back("*");

  const std::string start =
      _start_dir.empty() ? std::string(".") : _start_dir.string();
  std::vector<std::string> selection;
  try {
    selection =
        pfd::open_file(_title.empty() ? "Open file" : _title, start, filters)
            .result();
  } catch (const std::exception &) {
    return std::nullopt; // dialog backend unavailable/failed, not a user cancel
  }
  if (selection.empty()) {
    return std::nullopt;
  }
  return std::filesystem::path(selection.front());
}

} // namespace campcat
