#pragma once

#include <memory>
#include <string_view>

namespace campcat {

class adb_client;
struct app_config;
class automation_profile;
class game_automation;

/**
 * @brief Canonical `active_script_id` strings for bundled automation drivers.
 */
namespace shell_script_id {

inline constexpr std::string_view k_none = "none";
inline constexpr std::string_view k_ccat_script = "ccat_script";

} // namespace shell_script_id

/**
 * @brief One installable automation backend keyed by shell_script_id.
 */
class automation_driver {
public:
  virtual ~automation_driver() = default;

  virtual std::string_view script_id() const noexcept = 0;

  bool supports(std::string_view active_script_id) const noexcept {
    return active_script_id == script_id();
  }

  virtual std::unique_ptr<game_automation>
  create(adb_client *adb, const app_config *cfg,
         const automation_profile *bundle) const = 0;
};

} // namespace campcat
