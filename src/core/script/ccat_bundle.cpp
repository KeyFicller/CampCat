#include "core/script/ccat_bundle.h"

#include "core/script/ccat_parser.h"

#include <algorithm>
#include <fstream>
#include <iterator>
#include <system_error>

namespace campcat::ccat_lang {

std::filesystem::path bundle_main(const std::filesystem::path &_script_dir,
                                  std::string_view _name) {
  return _script_dir / std::string(_name) / "main.ccat";
}

std::vector<std::string> bundle_names(const std::filesystem::path &_script_dir) {
  std::vector<std::string> names;
  std::error_code ec;
  for (const auto &entry : std::filesystem::directory_iterator(_script_dir, ec)) {
    if (entry.is_directory(ec) &&
        std::filesystem::is_regular_file(entry.path() / "main.ccat", ec)) {
      names.push_back(entry.path().filename().string());
    }
  }
  std::sort(names.begin(), names.end());
  return names;
}

std::string bundle_description(const std::filesystem::path &_script_dir,
                               std::string_view _name) {
  std::ifstream in(bundle_main(_script_dir, _name), std::ios::binary);
  std::string line;
  if (!in || !std::getline(in, line)) {
    return {};
  }
  const std::size_t first = line.find_first_not_of(" \t");
  if (first == std::string::npos || line.compare(first, 2, "//") != 0) {
    return {};
  }
  const std::size_t start = line.find_first_not_of(" \t", first + 2);
  if (start == std::string::npos) {
    return {};
  }
  const std::size_t end = line.find_last_not_of(" \t\r");
  return line.substr(start, end - start + 1);
}

std::vector<std::string> bundle_templates(const std::filesystem::path &_script_dir,
                                          std::string_view _name) {
  std::vector<std::string> names;
  std::error_code ec;
  for (const auto &entry :
       std::filesystem::directory_iterator(_script_dir / std::string(_name), ec)) {
    if (entry.is_regular_file(ec) && entry.path().extension() == ".png") {
      names.push_back(entry.path().filename().string());
    }
  }
  std::sort(names.begin(), names.end());
  return names;
}

save_status save_bundle_main(const std::filesystem::path &_script_dir,
                             std::string_view _name, const std::string &_text,
                             const std::string &_loaded, std::string *_err) {
  const auto fail = [&_err](save_status _status, const std::string &_why) {
    if (_err != nullptr) {
      *_err = _why;
    }
    return _status;
  };

  try {
    parse_program(_text);
  } catch (const parse_error &_e) {
    return fail(save_status::parse_error,
                "line " + std::to_string(_e.line) + ", col " +
                    std::to_string(_e.col) + ": " + _e.what());
  }

  const std::filesystem::path path = bundle_main(_script_dir, _name);
  {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
      return fail(save_status::write_failed, "could not read " + path.string());
    }
    const std::string disk{std::istreambuf_iterator<char>(in),
                           std::istreambuf_iterator<char>()};
    if (disk != _loaded) {
      return fail(save_status::changed_on_disk,
                  "changed on disk since this window opened it");
    }
  }

  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  out.write(_text.data(), static_cast<std::streamsize>(_text.size()));
  out.close();
  if (!out) {
    return fail(save_status::write_failed, "could not write " + path.string());
  }
  return save_status::ok;
}

} // namespace campcat::ccat_lang
