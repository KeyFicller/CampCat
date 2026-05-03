#pragma once

namespace campcat {

/**
 * @brief Polymorphic root for persisted per-script bundle data.
 */
class automation_profile {
public:
  virtual ~automation_profile() = default;

  automation_profile() = default;
  automation_profile(const automation_profile &) = default;
  automation_profile &operator=(const automation_profile &) = default;
  automation_profile(automation_profile &&) = default;
  automation_profile &operator=(automation_profile &&) = default;
};

} // namespace campcat
