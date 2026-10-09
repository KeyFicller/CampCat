#pragma once

// JSON Schema generation for LLM tool declarations, from C++26 static
// reflection (P2996). Private to src/core/llm: <meta> and the consteval cost
// stay out of the public include/ tree.
//
// A tool is an ordinary free function. Its signature *is* the schema the model
// sees, so there is exactly one place to edit when a tool changes. The shape
// follows the working experiment at KeyFicller/CppReflect26; the differences
// are the strict argument binding (section "binding") and the narrower type
// support.
//
// Measured constraints of this compiler; do not "simplify" them away:
//   * `std::meta::parameters_of(Fn)` takes the info directly. Writing `^^Fn`
//     fails with "reflection operand must be a named entity".
//   * A reflection range must be fed to `template for` inline as
//     `std::define_static_array(...)`. Storing it in a `constexpr auto` first
//     fails: "pointer to subobject of heap-allocated object is not a constant
//     expression".
//   * Annotation tests use `if constexpr`. `if consteval` is false inside a
//     plain function, so the loop body silently never runs.
//   * `identifier_of` / `display_string_of` return string_view; feeding one to
//     printf("%s") compiles and then segfaults at runtime.
//   * Handing a consteval result to runtime code needs `define_static_string`.

#include <meta>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

namespace campcat::llm::js {

// --- annotation value types -------------------------------------------------
// An annotation value must be structural, so a string cannot be a string_view.
// str<N> holds exactly N characters including the NUL, e.g. str<6>{"hello"}.
template <std::size_t N>
struct str {
  char data[N]{};
  constexpr str() = default;
  constexpr str(const char (&_s)[N]) {
    for (std::size_t i = 0; i < N; ++i) {
      data[i] = _s[i];
    }
  }
};
template <std::size_t N>
str(const char (&)[N]) -> str<N>;

/// Documentation for a tool, applied on the function: [[= js::doc{.text = ...}]].
template <std::size_t N = 1>
struct doc {
  str<N> text;
};

/// A structural, variadic list of strings. std::tuple is not structural here,
/// so the list is rolled by hand.
template <typename... Ts>
struct doc_list;
template <>
struct doc_list<> {};
template <typename T, typename... Ts>
struct doc_list<T, Ts...> {
  T head;
  doc_list<Ts...> tail;
};

/// Parameter documentation, matched to parameters by position:
///   [[= js::param_docs(js::str("first"), js::str("second"))]]
/// An annotation on the parameter itself is not readable here, so the metadata
/// lives on the function.
consteval auto param_docs(auto... _xs) {
  return doc_list<decltype(_xs)...>{_xs...};
}

/// Marks a tool whose result is an image rather than text: [[= js::image_result]].
struct image_result_t {};
inline constexpr image_result_t image_result{};

/// Marks a tool whose result is a document for the model rather than log
/// material, i.e. text too long to print, and carries the line the log shows
/// instead: [[= js::quiet{.text = js::str("Read CCAT.md")}]]. The text itself
/// still reaches the model unchanged. Same shape as `doc` above, so
/// `text_of<^^quiet, Fn>()` reads it.
template <std::size_t N = 1>
struct quiet {
  str<N> text;
};

// --- type traits ------------------------------------------------------------

template <typename T>
struct is_optional : std::false_type {};
template <typename T>
struct is_optional<std::optional<T>> : std::true_type {};

// --- JSON helpers -----------------------------------------------------------

// constexpr: schema assembly happens inside a consteval context.
constexpr std::string json_escape(std::string_view _s) {
  std::string out;
  for (char c : _s) {
    switch (c) {
    case '"': out += "\\\""; break;
    case '\\': out += "\\\\"; break;
    case '\n': out += "\\n"; break;
    case '\r': out += "\\r"; break;
    case '\t': out += "\\t"; break;
    default: out += c; break;
    }
  }
  return out;
}

// --- binding: JSON value -> C++ value ---------------------------------------
// This is a trust boundary. The model's `arguments` arrive as text that nothing
// validates on the Python side (tools are bound as raw OpenAI dicts, so
// LangChain has no args_schema to check against), and a silently defaulted
// coordinate is a real mistap: a `tap` that lost its `x` would hit (0, y).

struct bind_error : std::runtime_error {
  using std::runtime_error::runtime_error;
};

/// Accepts only the exact JSON type. "540" is an error, not a coercion.
template <typename T>
T from_json(const nlohmann::json &_j) {
  if constexpr (is_optional<T>::value) {
    if (_j.is_null()) {
      return std::nullopt;
    }
    return std::optional{from_json<typename T::value_type>(_j)};
  } else if constexpr (std::is_same_v<T, bool>) {
    if (!_j.is_boolean()) {
      throw bind_error("expected boolean");
    }
    return _j.get<bool>();
  } else if constexpr (std::is_integral_v<T>) {
    if (!_j.is_number_integer()) {
      throw bind_error("expected integer");
    }
    return _j.get<T>();
  } else if constexpr (std::is_floating_point_v<T>) {
    if (!_j.is_number()) {
      throw bind_error("expected number");
    }
    return _j.get<T>();
  } else if constexpr (std::is_same_v<T, std::string>) {
    if (!_j.is_string()) {
      throw bind_error("expected string");
    }
    return _j.get<std::string>();
  } else {
    static_assert(!sizeof(T), "js::from_json: unsupported parameter type "
                              "(add a branch when a tool needs it)");
  }
}

// --- annotations ------------------------------------------------------------

namespace detail {
namespace meta = std::meta;
} // namespace detail

/// First annotation on `_e` whose type is an instantiation of `Tmpl`, or an
/// empty reflection when there is none.
template <detail::meta::info Tmpl>
consteval detail::meta::info find_annotation(detail::meta::info _e) {
  for (detail::meta::info a : detail::meta::annotations_of(_e)) {
    const detail::meta::info t = detail::meta::remove_const(detail::meta::type_of(a));
    if (detail::meta::has_template_arguments(t) && detail::meta::template_of(t) == Tmpl) {
      return a;
    }
  }
  return {};
}

/// Text held by the first `Tmpl` annotation on `E`, or "" when there is none.
template <detail::meta::info Tmpl, detail::meta::info E>
consteval std::string text_of() {
  if constexpr (find_annotation<Tmpl>(E) == detail::meta::info{}) {
    return {};
  } else {
    constexpr auto cfg = detail::meta::extract<
        typename[: detail::meta::type_of(find_annotation<Tmpl>(E)) :]>(find_annotation<Tmpl>(E));
    return std::string(std::string_view(cfg.text.data));
  }
}

template <detail::meta::info Fn>
consteval std::size_t param_doc_count() {
  if (find_annotation<^^doc_list>(Fn) == detail::meta::info{}) {
    return 0;
  }
  return detail::meta::template_arguments_of(
             detail::meta::remove_const(detail::meta::type_of(find_annotation<^^doc_list>(Fn))))
      .size();
}

template <std::size_t I, typename List>
consteval auto nth_doc(List _list) {
  if constexpr (I == 0) {
    return _list.head;
  } else {
    return nth_doc<I - 1>(_list.tail);
  }
}

/// Positional description for parameter `I` of `Fn`, or "" if undocumented.
template <detail::meta::info Fn, std::size_t I>
consteval std::string param_description() {
  if constexpr (find_annotation<^^doc_list>(Fn) == detail::meta::info{}) {
    return {};
  } else if constexpr (I < param_doc_count<Fn>()) {
    constexpr auto list = detail::meta::extract<
        typename[: detail::meta::type_of(find_annotation<^^doc_list>(Fn)) :]>(
        find_annotation<^^doc_list>(Fn));
    return std::string(std::string_view(nth_doc<I>(list).data));
  } else {
    return {};
  }
}

/// True when `_e` carries an annotation whose const-removed type is exactly
/// `Tmpl`. `find_annotation` above only matches template instantiations, so the
/// plain marker annotations need this loop instead.
template <detail::meta::info Tmpl>
consteval bool has_annotation(detail::meta::info _e) {
  for (detail::meta::info a : detail::meta::annotations_of(_e)) {
    if (detail::meta::remove_const(detail::meta::type_of(a)) == Tmpl) {
      return true;
    }
  }
  return false;
}

/// True when the tool's result is an image, i.e. it carries [[= js::image_result]].
template <detail::meta::info Fn>
consteval bool has_image_annotation() {
  return has_annotation<^^image_result_t>(Fn);
}

/// The line the UI log shows in place of a quiet tool's result, i.e. the text of
/// its [[= js::quiet]] annotation. Empty when the tool carries no such annotation,
/// which is also the signal that it is not quiet: a non-empty value is what
/// replaces the result in the log. `define_static_string` because the result has
/// to outlive the consteval call. Wrapped in its own function because the
/// registration macro expands outside this namespace, where `^^quiet` would not
/// resolve unqualified.
template <detail::meta::info Fn>
consteval std::string_view quiet_note_of() {
  return std::string_view(std::define_static_string(text_of<^^quiet, Fn>()));
}

// --- schema generation ------------------------------------------------------

/// Splices a description into a generated schema, which is always one object.
consteval std::string with_description(std::string _schema, std::string_view _desc) {
  if (_desc.empty()) {
    return _schema;
  }
  return _schema.substr(0, _schema.size() - 1) + ",\"description\":\"" +
         json_escape(_desc) + "\"}";
}

/// JSON Schema for one C++ type. std::optional unwraps to its value type.
/// Anything else is a deliberate compile error: the schema and the binder must
/// gain matching support together, never one without the other.
template <typename T>
consteval std::string param_schema() {
  if constexpr (is_optional<T>::value) {
    return param_schema<typename T::value_type>();
  } else if constexpr (std::is_same_v<T, bool>) {
    return "{\"type\":\"boolean\"}";
  } else if constexpr (std::is_integral_v<T>) {
    return "{\"type\":\"integer\"}";
  } else if constexpr (std::is_floating_point_v<T>) {
    return "{\"type\":\"number\"}";
  } else if constexpr (std::is_same_v<T, std::string> ||
                       std::is_same_v<T, std::string_view>) {
    return "{\"type\":\"string\"}";
  } else {
    static_assert(!sizeof(T), "js::param_schema: unsupported parameter type "
                              "(add a branch when a tool needs it)");
  }
}

// --- function introspection -------------------------------------------------
// Everything touching meta::info happens at consteval. The results handed to
// the runtime string assembly are plain values.

template <detail::meta::info Fn>
consteval std::size_t param_count() {
  return detail::meta::parameters_of(Fn).size();
}

template <detail::meta::info Fn, std::size_t I>
consteval detail::meta::info param_at() {
  return detail::meta::parameters_of(Fn)[I];
}

template <detail::meta::info Fn, std::size_t I>
using param_type_at = std::remove_cvref_t<typename[: detail::meta::type_of(param_at<Fn, I>()) :]>;

/// `required` lists only the parameters the model must supply: an optional
/// parameter may be omitted, and the tool body then applies its own default.
template <detail::meta::info Fn, std::size_t... Is>
consteval std::string param_object_schema_impl(std::index_sequence<Is...>) {
  std::string properties;
  std::string required;
  ((void)([&] {
     using PT = param_type_at<Fn, Is>;
     const std::string name(detail::meta::identifier_of(param_at<Fn, Is>()));
     if (!properties.empty()) {
       properties += ",";
     }
     properties += "\"" + name + "\":" +
                   with_description(param_schema<PT>(), param_description<Fn, Is>());
     if constexpr (!is_optional<PT>::value) {
       if (!required.empty()) {
         required += ",";
       }
       required += "\"" + name + "\"";
     }
   }()), ...);
  return "{\"type\":\"object\",\"properties\":{" + properties +
         "},\"required\":[" + required + "]}";
}

template <detail::meta::info Fn>
consteval std::string param_object_schema() {
  return param_object_schema_impl<Fn>(std::make_index_sequence<param_count<Fn>()>());
}

/// The complete OpenAI tool for `Fn`, materialized for runtime use.
template <detail::meta::info Fn>
consteval std::string_view tool_schema() {
  std::string out = "{\"type\":\"function\",\"function\":{\"name\":\"";
  out += detail::meta::identifier_of(Fn);
  out += "\",\"description\":\"" + json_escape(text_of<^^doc, Fn>()) + "\",\"parameters\":";
  out += param_object_schema<Fn>();
  out += "}}";
  return std::string_view(std::define_static_string(out));
}

// --- invocation -------------------------------------------------------------

/// Binds parameter `I` from the model's arguments. A missing required argument
/// is an error rather than a default-constructed value.
template <detail::meta::info Fn, std::size_t I>
void bind_one(auto &_args, const nlohmann::json &_j) {
  using PT = param_type_at<Fn, I>;
  const std::string key(detail::meta::identifier_of(param_at<Fn, I>()));
  const auto it = _j.find(key);
  if (it == _j.end()) {
    if constexpr (is_optional<PT>::value) {
      std::get<I>(_args) = PT{};
      return;
    } else {
      throw bind_error("missing required argument: " + key);
    }
  }
  try {
    std::get<I>(_args) = from_json<PT>(*it);
  } catch (const bind_error &e) {
    throw bind_error(key + ": " + e.what());
  }
}

template <detail::meta::info Fn, std::size_t... Is>
std::string invoke_strict_impl(const nlohmann::json &_j, std::index_sequence<Is...>) {
  std::tuple<param_type_at<Fn, Is>...> args{};
  (bind_one<Fn, Is>(args, _j), ...);
  return std::apply(&[: Fn :], args);
}

/// Builds the argument tuple and performs the real C++ call. Throws bind_error
/// on a bad argument and whatever the tool body throws otherwise.
template <detail::meta::info Fn>
std::string invoke_strict(const nlohmann::json &_j) {
  return invoke_strict_impl<Fn>(_j, std::make_index_sequence<param_count<Fn>()>());
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
    return ::campcat::llm::js::invoke_strict<^^fn>(_j);                            \
  }                                                                                \
  [[maybe_unused]] const int js_reg_tool_##fn = [] {                               \
    ::campcat::llm::js::register_tool(::campcat::llm::js::tool_entry{              \
        #fn, ::campcat::llm::js::tool_schema<^^fn>(), &js_run_##fn,                \
        ::campcat::llm::js::has_image_annotation<^^fn>(),                          \
        ::campcat::llm::js::quiet_note_of<^^fn>()});                               \
    return 0;                                                                      \
  }();                                                                             \
  }
