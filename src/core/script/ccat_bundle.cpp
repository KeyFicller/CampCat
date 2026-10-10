#include "core/script/ccat_bundle.h"

#include "core/script/ccat_lexer.h"  // Lexer: a run() target is edited as source text
#include "core/script/ccat_parser.h"

#include <algorithm>
#include <cstddef>
#include <fstream>
#include <iterator>
#include <memory>
#include <optional>
#include <system_error>

namespace campcat::ccat_lang {

bool safe_leaf_name(std::string_view _name) {
  if (_name.empty() || _name == "." || _name == "..") {
    return false;
  }
  for (const unsigned char c : _name) {
    if (c == '/' || c == '\\' || c < 0x20U || c == 0x7FU) {
      return false;
    }
  }
  return true;
}

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

namespace {

/// The script root as a canonical path, so a resolved target can be compared
/// against it. Falls back to the path as given when canonicalization fails.
std::filesystem::path canonical_root(const std::filesystem::path &_script_dir) {
  std::error_code ec;
  const std::filesystem::path canon =
      std::filesystem::weakly_canonical(_script_dir, ec);
  return ec ? _script_dir : canon;
}

/// ponytail: a statement type that can hold a body has to be walked here, or a
/// run() nested inside it becomes an edge nobody sees and its target gets
/// deleted. New statement types only appear in the parser, so nothing can slip
/// in unseen.
void collect_run_targets(const Stmt &_s, std::vector<std::string> *_out) {
  if (const auto *b = dynamic_cast<const BlockStmt *>(&_s)) {
    for (const auto &child : b->body) {
      collect_run_targets(*child, _out);
    }
  } else if (const auto *i = dynamic_cast<const IfStmt *>(&_s)) {
    if (i->then_branch) {
      collect_run_targets(*i->then_branch, _out);
    }
    if (i->else_branch) {
      collect_run_targets(*i->else_branch, _out);
    }
  } else if (const auto *r = dynamic_cast<const RetryStmt *>(&_s)) {
    if (r->body) {
      collect_run_targets(*r->body, _out);
    }
  } else if (const auto *d = dynamic_cast<const DoWhileStmt *>(&_s)) {
    if (d->body) {
      collect_run_targets(*d->body, _out);
    }
  } else if (const auto *l = dynamic_cast<const LoopStmt *>(&_s)) {
    if (l->body) {
      collect_run_targets(*l->body, _out);
    }
  } else if (const auto *t = dynamic_cast<const RunStmt *>(&_s)) {
    _out->push_back(t->script_rel);
  }
}

/// What one bundle's main.ccat says about its dependencies, or why it cannot say.
struct bundle_scan {
  bool parsed = false;
  std::vector<std::string> deps; ///< bundle names, sorted, no duplicates
  std::string why;               ///< set when !parsed
};

/// The bundles `_name` reaches with run(). `_known` is the sorted list
/// `bundle_names` gave: a target landing in no bundle is not an edge.
bundle_scan scan_bundle(const std::filesystem::path &_script_dir,
                        const std::string &_name,
                        const std::vector<std::string> &_known) {
  bundle_scan out;
  const std::filesystem::path path = bundle_main(_script_dir, _name);
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    out.why = "could not read " + path.string();
    return out;
  }

  std::unique_ptr<Program> prog;
  try {
    prog = parse_program(std::string{std::istreambuf_iterator<char>(in),
                                     std::istreambuf_iterator<char>()});
  } catch (const parse_error &_e) {
    out.why = _name + "/main.ccat does not parse (line " +
              std::to_string(_e.line) + ", col " + std::to_string(_e.col) + ")";
    return out;
  }
  out.parsed = true;

  std::vector<std::string> targets;
  for (const auto &st : prog->stmts) {
    collect_run_targets(*st, &targets);
  }

  const std::filesystem::path root = canonical_root(_script_dir);
  const std::filesystem::path base = _script_dir / _name;
  for (const std::string &target : targets) {
    // A target outside the root leaves a ".." first, which no bundle is named.
    const std::filesystem::path rel =
        resolve_script_path_from(base, target).lexically_relative(root);
    const auto first = rel.begin();
    if (first == rel.end()) {
      continue;
    }
    // Self-references are not edges: a bundle cannot keep itself alive.
    const std::string owner = first->string();
    if (owner != _name &&
        std::binary_search(_known.begin(), _known.end(), owner)) {
      out.deps.push_back(owner);
    }
  }
  std::sort(out.deps.begin(), out.deps.end());
  out.deps.erase(std::unique(out.deps.begin(), out.deps.end()), out.deps.end());
  return out;
}

/// Joins names for a sentence a person reads.
std::string join(const std::vector<std::string> &_names) {
  std::string out;
  for (const std::string &name : _names) {
    if (!out.empty()) {
      out += ", ";
    }
    out += name;
  }
  return out;
}

} // namespace

std::filesystem::path resolve_script_path_from(const std::filesystem::path &_base,
                                               const std::string &_rel) {
  std::filesystem::path p(_rel);
  if (!p.is_absolute()) {
    p = _base / p;
  }
  std::error_code ec;
  const std::filesystem::path canon = std::filesystem::weakly_canonical(p, ec);
  return ec ? p.lexically_normal() : canon;
}

std::vector<std::string> bundle_dependencies(const std::filesystem::path &_script_dir,
                                             std::string_view _name) {
  return scan_bundle(_script_dir, std::string(_name), bundle_names(_script_dir))
      .deps;
}

std::vector<std::string> bundle_referrers(const std::filesystem::path &_script_dir,
                                          std::string_view _name) {
  const std::vector<std::string> known = bundle_names(_script_dir);
  const std::string wanted(_name);
  std::vector<std::string> out;
  for (const std::string &other : known) {
    if (other == wanted) {
      continue;
    }
    const std::vector<std::string> deps =
        scan_bundle(_script_dir, other, known).deps;
    if (std::binary_search(deps.begin(), deps.end(), wanted)) {
      out.push_back(other);
    }
  }
  return out;
}

delete_status check_bundle_deletable(const std::filesystem::path &_script_dir,
                                     std::string_view _name, std::string *_err) {
  const auto fail = [&_err](delete_status _status, const std::string &_why) {
    if (_err != nullptr) {
      *_err = _why;
    }
    return _status;
  };

  const std::string name(_name);
  std::error_code ec;
  if (!std::filesystem::is_regular_file(bundle_main(_script_dir, name), ec)) {
    const std::string existing = join(bundle_names(_script_dir));
    return fail(delete_status::not_found,
                "no script bundle \"" + name +
                    "\" (existing: " + (existing.empty() ? "none yet" : existing) +
                    ")");
  }

  const std::vector<std::string> known = bundle_names(_script_dir);
  std::vector<std::string> referrers;
  for (const std::string &other : known) {
    if (other == name) {
      continue;
    }
    const bundle_scan scan = scan_bundle(_script_dir, other, known);
    // A script that will not parse might still hold a run() aimed here, and
    // deleting would take the target with it, so refuse rather than guess.
    if (!scan.parsed) {
      return fail(delete_status::scan_failed,
                  "cannot check for references: " + scan.why);
    }
    if (std::binary_search(scan.deps.begin(), scan.deps.end(), name)) {
      referrers.push_back(other);
    }
  }
  if (!referrers.empty()) {
    const bool one = referrers.size() == 1;
    return fail(delete_status::referenced,
                "cannot delete " + name + ": run(...) from " + join(referrers) +
                    (one ? " reaches it" : " reach it") + "; update or delete " +
                    (one ? "that script" : "those scripts") + " first");
  }
  return delete_status::ok;
}

delete_status delete_bundle(const std::filesystem::path &_script_dir,
                            std::string_view _name, std::string *_err) {
  const delete_status checked = check_bundle_deletable(_script_dir, _name, _err);
  if (checked != delete_status::ok) {
    return checked;
  }
  const std::filesystem::path dir = _script_dir / std::string(_name);
  std::error_code ec;
  std::filesystem::remove_all(dir, ec);
  if (ec) {
    if (_err != nullptr) {
      *_err = "could not delete " + dir.string();
    }
    return delete_status::remove_failed;
  }
  return delete_status::ok;
}

namespace {

/// `line_starts[i]` is the byte offset where line `i+1` begins. A Token carries a
/// line and a column, and a column counts bytes (the lexer advances one per
/// byte), so this is what turns a Token into a span of source text.
std::vector<std::size_t> line_starts(std::string_view _src) {
  std::vector<std::size_t> starts{0};
  for (std::size_t i = 0; i < _src.size(); ++i) {
    if (_src[i] == '\n') {
      starts.push_back(i + 1);
    }
  }
  return starts;
}

/// The offset just past the closing quote of the string starting at `_open`,
/// scanned with the same escape rule as Lexer::lex_string. Returns `_src.size()`
/// if it never closes, which a token the lexer called Str cannot be.
std::size_t string_end(std::string_view _src, std::size_t _open) {
  for (std::size_t i = _open + 1; i < _src.size(); ++i) {
    if (_src[i] == '\\' && i + 1 < _src.size()) {
      ++i; // the escaped byte, whatever it is
      continue;
    }
    if (_src[i] == '"') {
      return i + 1;
    }
  }
  return _src.size();
}

/// One literal to swap out of a referrer's source.
struct literal_edit {
  std::size_t begin = 0;
  std::size_t end = 0;
  std::string text;
};

/// `_s` as a .ccat string literal, escapes included.
std::string quote_path(const std::string &_s) {
  std::string out = "\"";
  for (const char c : _s) {
    if (c == '\\' || c == '"') {
      out += '\\';
    }
    out += c;
  }
  out += '"';
  return out;
}

/// Rewrites every `run()` literal in `_text` that resolves into bundle `_from`
/// so that it resolves into `_to` instead, the same file inside it. Returns no
/// value when there is nothing to rewrite -- which, for a script known to refer
/// to `_from`, means its target reached through a `defs` name rather than a
/// literal, and there is no source text to edit.
std::optional<std::string> rewrite_run_targets(
    const std::filesystem::path &_script_dir, const std::string &_referrer,
    std::string_view _text, const std::string &_from, const std::string &_to) {
  Lexer lex(_text);
  std::vector<Token> toks;
  for (Token t = lex.peek(); t.kind != TokKind::Eof; t = lex.next()) {
    toks.push_back(t);
  }
  const std::vector<std::size_t> starts = line_starts(_text);

  const std::filesystem::path root = canonical_root(_script_dir);
  // Both sides of every comparison below come off `root`: lexically_relative
  // finds no common base between a canonical path and a relative one, so mixing
  // the caller's `_script_dir` in here would silently match nothing.
  const std::filesystem::path from_dir = root / _from;
  const std::filesystem::path referrer_dir = root / _referrer;

  std::vector<literal_edit> edits;
  for (std::size_t i = 0; i + 2 < toks.size(); ++i) {
    if (toks[i].kind != TokKind::KwRun || toks[i + 1].kind != TokKind::LParen ||
        toks[i + 2].kind != TokKind::Str) {
      continue;
    }
    const Token &tok = toks[i + 2];
    const std::filesystem::path abs =
        resolve_script_path_from(referrer_dir, tok.text);

    const std::filesystem::path inside = abs.lexically_relative(root);
    const auto first = inside.begin();
    if (first == inside.end() || first->string() != _from) {
      continue;
    }
    // The file to carry over, taken relative to the bundle. A target that is not
    // underneath it (a symlink, say) has nothing to carry: refuse the rename
    // rather than guess at a path.
    const std::filesystem::path suffix = abs.lexically_relative(from_dir);
    const auto suffix_first = suffix.begin();
    if (suffix_first == suffix.end() || suffix_first->string() == "..") {
      return std::nullopt;
    }

    const std::filesystem::path new_abs = root / _to / suffix;
    std::string replacement;
    if (std::filesystem::path(tok.text).is_absolute()) {
      replacement = new_abs.string(); // an absolute target stays absolute
    } else {
      // Lexical, not std::filesystem::relative: that one canonicalizes, and a
      // canonical path comes back spelled the way the filesystem has it, so a
      // rename that only changes case would write the old spelling back in.
      replacement = new_abs.lexically_relative(referrer_dir).string();
    }

    if (tok.line < 1 || static_cast<std::size_t>(tok.line) > starts.size()) {
      return std::nullopt;
    }
    const std::size_t open_off = starts[static_cast<std::size_t>(tok.line) - 1] +
                                 static_cast<std::size_t>(tok.col - 1);
    // The column points at the opening quote; anything else means this is not
    // the span we think it is, and guessing would corrupt the file.
    if (open_off >= _text.size() || _text[open_off] != '"') {
      return std::nullopt;
    }
    edits.push_back(
        literal_edit{open_off, string_end(_text, open_off), quote_path(replacement)});
  }

  if (edits.empty()) {
    return std::nullopt;
  }

  // Tokens arrive in source order, so the spans do too; the guard catches a span
  // that ever is not where it should be rather than splicing nonsense.
  std::string out;
  std::size_t cursor = 0;
  for (const literal_edit &edit : edits) {
    if (edit.begin < cursor) {
      return std::nullopt;
    }
    out.append(_text.substr(cursor, edit.begin - cursor));
    out += edit.text;
    cursor = edit.end;
  }
  out.append(_text.substr(cursor));
  return out;
}

/// Everything a rename will do, worked out without touching a byte of it.
struct rename_plan {
  std::vector<std::string> referrers; ///< sorted
  std::vector<std::string> edits;     ///< parallel to referrers
  std::vector<std::string> old_texts; ///< parallel to referrers, for the write guard
};

rename_status plan_rename(const std::filesystem::path &_script_dir,
                          const std::string &_name, const std::string &_new_name,
                          rename_plan *_out, std::string *_err) {
  const auto fail = [&_err](rename_status _status, const std::string &_why) {
    if (_err != nullptr) {
      *_err = _why;
    }
    return _status;
  };

  if (!safe_leaf_name(_new_name)) {
    return fail(rename_status::bad_name,
                "\"" + _new_name + "\" is not a plain file name");
  }
  if (_new_name == _name) {
    return fail(rename_status::bad_name,
                "the bundle is already called \"" + _name + "\"");
  }

  std::error_code ec;
  if (!std::filesystem::is_regular_file(bundle_main(_script_dir, _name), ec)) {
    const std::string existing = join(bundle_names(_script_dir));
    return fail(rename_status::not_found,
                "no script bundle \"" + _name +
                    "\" (existing: " + (existing.empty() ? "none yet" : existing) +
                    ")");
  }

  // Compared by exact spelling rather than existence: on a case-insensitive
  // filesystem `exists` would call a case-only rename a collision, and a
  // directory of that exact name is occupied whether or not it holds a bundle.
  for (const auto &entry : std::filesystem::directory_iterator(_script_dir, ec)) {
    if (entry.is_directory(ec) && entry.path().filename().string() == _new_name) {
      return fail(rename_status::name_taken,
                  "a directory named \"" + _new_name + "\" already exists");
    }
  }

  const std::vector<std::string> known = bundle_names(_script_dir);
  std::vector<std::string> referrers;
  for (const std::string &other : known) {
    if (other == _name) {
      continue;
    }
    const bundle_scan scan = scan_bundle(_script_dir, other, known);
    if (!scan.parsed) {
      return fail(rename_status::scan_failed,
                  "cannot find the scripts that use it: " + scan.why);
    }
    if (std::binary_search(scan.deps.begin(), scan.deps.end(), _name)) {
      referrers.push_back(other);
    }
  }

  for (const std::string &referrer : referrers) {
    const std::filesystem::path path = bundle_main(_script_dir, referrer);
    std::ifstream in(path, std::ios::binary);
    if (!in) {
      return fail(rename_status::rewrite_failed,
                  "cannot rewrite " + path.string() + ": could not read it");
    }
    std::string text{std::istreambuf_iterator<char>(in),
                     std::istreambuf_iterator<char>()};

    const std::optional<std::string> rewritten =
        rewrite_run_targets(_script_dir, referrer, text, _name, _new_name);
    if (!rewritten.has_value()) {
      return fail(rename_status::rewrite_failed,
                  "cannot rewrite " + referrer +
                      ": its run() target is a defs name, not a literal path; "
                      "update it yourself first");
    }
    try {
      parse_program(*rewritten);
    } catch (const parse_error &_e) {
      return fail(rename_status::rewrite_failed,
                  "cannot rewrite " + referrer + "/main.ccat: line " +
                      std::to_string(_e.line) + ", col " + std::to_string(_e.col) +
                      ": " + _e.what());
    }

    _out->referrers.push_back(referrer);
    _out->old_texts.push_back(std::move(text));
    _out->edits.push_back(*rewritten);
  }
  return rename_status::ok;
}

} // namespace

rename_status check_bundle_renamable(const std::filesystem::path &_script_dir,
                                     std::string_view _name,
                                     std::string_view _new_name,
                                     std::vector<std::string> *_referrers,
                                     std::string *_err) {
  rename_plan plan;
  const rename_status status = plan_rename(
      _script_dir, std::string(_name), std::string(_new_name), &plan, _err);
  if (status == rename_status::ok && _referrers != nullptr) {
    *_referrers = plan.referrers;
  }
  return status;
}

rename_status rename_bundle(const std::filesystem::path &_script_dir,
                            std::string_view _name, std::string_view _new_name,
                            std::vector<std::string> *_rewritten,
                            std::string *_err) {
  const std::string name(_name);
  const std::string new_name(_new_name);
  rename_plan plan;
  const rename_status status = plan_rename(_script_dir, name, new_name, &plan, _err);
  if (status != rename_status::ok) {
    return status;
  }

  std::error_code ec;
  std::filesystem::rename(_script_dir / name, _script_dir / new_name, ec);
  if (ec) {
    if (_err != nullptr) {
      *_err = "could not rename " + (_script_dir / name).string() + ": " +
              ec.message();
    }
    return rename_status::rename_failed;
  }

  std::vector<std::string> stale;
  for (std::size_t i = 0; i < plan.referrers.size(); ++i) {
    std::string err;
    if (save_bundle_main(_script_dir, plan.referrers[i], plan.edits[i],
                         plan.old_texts[i], &err) != save_status::ok) {
      stale.push_back(plan.referrers[i] + " (" + err + ")");
    }
  }
  if (!stale.empty()) {
    // The move stands: the bundle is intact, these scripts still name the old one.
    if (_err != nullptr) {
      *_err = "renamed " + name + " to " + new_name +
              ", but these scripts still run \"" + name + "\": " + join(stale) +
              "; update them by hand";
    }
    return rename_status::rewrite_failed;
  }

  if (_rewritten != nullptr) {
    *_rewritten = plan.referrers;
  }
  return rename_status::ok;
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
