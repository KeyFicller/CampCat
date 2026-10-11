#pragma once

// The tool layer: what a CampCat tool is, plus the two annotations that describe
// how its *result* is presented -- [[= js::image_result]] and [[= js::quiet]].
//
// The generic reflection machinery (annotation types, schema generation, strict
// JSON binding) now lives in CppReflect26 and is re-exported below, so `js::doc`
// and friends read exactly as they did when this file owned them.
//
// The re-exports are `using` declarations, not copies: `campcat::llm::js::doc` is
// the same entity as `::js::doc`, so an annotation written on a tool is found by
// the library's `text_of<^^js::doc, Fn>()`. They have to be spelled out because a
// qualified name like `js::doc` stops at the first enclosing namespace called
// `js`; without them the tools' annotations would not resolve.

#include <meta>

#include <llm/reflection_json.h>

#include <array>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace campcat::llm::js {

// --- the generic layer, from CppReflect26 -----------------------------------
using ::js::bind_error;
using ::js::doc;
using ::js::invoke_with_json;
using ::js::is_optional;
using ::js::param_at;
using ::js::param_count;
using ::js::param_docs;
using ::js::param_type_at;
using ::js::str;
using ::js::text_of;

// --- CampCat-only annotations -----------------------------------------------

/// Marks a tool whose result is an image rather than text: [[= js::image_result]].
struct image_result_t {};
inline constexpr image_result_t image_result{};

/// Marks a tool whose result is a document for the model rather than log
/// material, i.e. text too long to print, and carries the line the log shows
/// instead: [[= js::quiet{.text = js::str("Read CCAT.md")}]]. The text itself
/// still reaches the model unchanged. Same shape as `doc`, so
/// `text_of<^^quiet, Fn>()` reads it.
///
/// `{param}` in that text names one of the tool's own parameters and is filled
/// from the call's arguments, so the log can say which script was read rather
/// than only that one was. Checked when the tool registers: a name that is not a
/// parameter of the tool, or one whose type cannot be printed (a vector, a
/// struct), fails the build. A param that is optional and absent prints nothing.
template <std::size_t N = 1>
struct quiet {
  str<N> text;
};

/// True when `_e` carries an annotation whose const-removed type is exactly
/// `Tmpl`. The library's `find_annotation` only matches template instantiations,
/// so the plain marker annotations need this loop instead.
template <std::meta::info Tmpl>
consteval bool has_annotation(std::meta::info _e) {
  for (std::meta::info a : std::meta::annotations_of(_e)) {
    if (std::meta::remove_const(std::meta::type_of(a)) == Tmpl) {
      return true;
    }
  }
  return false;
}

/// True when the tool's result is an image, i.e. it carries [[= js::image_result]].
template <std::meta::info Fn>
consteval bool has_image_annotation() {
  return has_annotation<^^image_result_t>(Fn);
}

/// True when a parameter can be written into a one-line log message: text and
/// numbers can, an optional of either can, and everything else (a vector, a
/// struct) would need a format nobody asked for. Reflecting those would also be
/// wrong: it would print the fields rather than the value.
template <typename T>
consteval bool quiet_renderable() {
  if constexpr (is_optional<T>::value) {
    return quiet_renderable<typename T::value_type>();
  } else {
    return std::is_same_v<T, std::string> || std::is_same_v<T, std::string_view> ||
           std::is_arithmetic_v<T>;
  }
}

/// One bool per parameter, in declaration order. A template fold so the splice
/// in `param_type_at` only ever sees a constant index.
template <std::meta::info Fn, std::size_t... Is>
consteval std::array<bool, sizeof...(Is)> quiet_renderable_table(std::index_sequence<Is...>) {
  return {quiet_renderable<param_type_at<Fn, Is>>()...};
}

/// The parameter names, in declaration order: the same fold shape, because
/// `param_at` is a template and cannot be indexed with a loop variable.
template <std::meta::info Fn, std::size_t... Is>
consteval std::array<std::string_view, sizeof...(Is)> quiet_param_names(std::index_sequence<Is...>) {
  return {std::meta::identifier_of(param_at<Fn, Is>())...};
}

template <std::meta::info Fn>
consteval bool quiet_note_ok() {
  const std::string note = text_of<^^quiet, Fn>();
  constexpr std::size_t count = param_count<Fn>();
  constexpr auto renderable = quiet_renderable_table<Fn>(std::make_index_sequence<count>{});
  constexpr auto names = quiet_param_names<Fn>(std::make_index_sequence<count>{});
  std::size_t at = 0;
  while (at < note.size()) {
    const std::size_t open = note.find('{', at);
    if (open == std::string::npos) {
      break;
    }
    const std::size_t close = note.find('}', open);
    if (close == std::string::npos) {
      return false; // an unclosed brace is a typo, not text
    }
    const std::string key = note.substr(open + 1, close - open - 1);
    bool ok = false;
    for (std::size_t i = 0; i < count; ++i) {
      if (names[i] == key) {
        ok = renderable[i];
        break;
      }
    }
    if (!ok) {
      return false;
    }
    at = close + 1;
  }
  return true;
}

/// The line the UI log shows in place of a quiet tool's result, i.e. the text of
/// its [[= js::quiet]] annotation. Empty when the tool carries no such annotation,
/// which is also the signal that it is not quiet: a non-empty value is what
/// replaces the result in the log. `define_static_string` because the result has
/// to outlive the consteval call. Wrapped in its own function because the
/// registration macro expands outside this namespace, where `^^quiet` would not
/// resolve unqualified.
template <std::meta::info Fn>
consteval std::string_view quiet_note_of() {
  static_assert(quiet_note_ok<Fn>(),
                "js::quiet: every {name} must be a parameter of this tool whose "
                "type can be printed as text (text or number)");
  return std::string_view(std::define_static_string(text_of<^^quiet, Fn>()));
}

// --- registry ---------------------------------------------------------------

struct tool_entry {
  std::string_view name;
  std::string_view schema;
  std::string (*run)(const nlohmann::json &);
  bool returns_image = false;
  /// What the log shows in place of this tool's result. Non-empty also marks the
  /// tool quiet: the result is a document for the model, too long to print.
  std::string_view quiet_note;
};

inline std::vector<tool_entry> &tool_registry() {
  // Function-local static: registration order across translation units is safe.
  static std::vector<tool_entry> entries;
  return entries;
}

/// Names must be unique: an OpenAI tools array with a duplicate name is
/// rejected outright, and a duplicate would silently collapse in the registry.
/// Registration happens in static initializers, so this fires before main().
inline void register_tool(tool_entry _entry) {
  for (const auto &existing : tool_registry()) {
    if (existing.name == _entry.name) {
      std::fputs("js: duplicate tool name (names must be unique)\n", stderr);
      std::abort();
    }
  }
  tool_registry().push_back(std::move(_entry));
}

/// The whole registry as the OpenAI tools array the sidecar hands to bind_tools.
inline std::string tools_json() {
  std::string out = "[";
  bool first = true;
  for (const tool_entry &entry : tool_registry()) {
    if (!first) {
      out += ",";
    }
    first = false;
    out += std::string(entry.schema);
  }
  out += "]";
  return out;
}

} // namespace campcat::llm::js

/// Registers `fn` as a tool named after itself. Colocated with the definition,
/// inside the same namespace.
#define CPP_REFLECT_TOOL(fn)                                                       \
  namespace {                                                                      \
  std::string js_run_##fn(const nlohmann::json &_j) {                              \
    return ::js::invoke_with_json<^^fn>(_j);                                       \
  }                                                                                \
  [[maybe_unused]] const int js_reg_tool_##fn = [] {                               \
    ::campcat::llm::js::register_tool(::campcat::llm::js::tool_entry{              \
        #fn, ::js::function_schema<^^fn, ::js::Style::OpenAiTool>(), &js_run_##fn, \
        ::campcat::llm::js::has_image_annotation<^^fn>(),                          \
        ::campcat::llm::js::quiet_note_of<^^fn>()});                               \
    return 0;                                                                      \
  }();                                                                             \
  }
