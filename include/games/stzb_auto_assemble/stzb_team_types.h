#pragma once

namespace campcat_stzb {

/**
 * @brief Recruitment-slot heuristic buckets used only by STZB scripting.
 */
enum class team_recruit_state {
  unknown,
  idle,
  in_progress,
  full,
};

/**
 * @brief Per-slot classification payload for assemble automation.
 */
struct team_recruit_status {
  team_recruit_state state = team_recruit_state::unknown;
  double progress_estimate = 0.0;
};

} // namespace campcat_stzb
