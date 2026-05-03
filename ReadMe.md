Mac（及部分 POSIX）下、面向 **Android 模拟器** 的STZB挂机小助手：通过 **ADB 截图 + OpenCV 模板匹配** 轮询主城征兵状态，并为满足条件的队伍完成预设的编队 / 集结相关自动化点击；适合躺尸挂机、且盟里有活跃指挥时使用。

## 功能概要

- **定时调度**：可配置间隔与可选抖动，周期性执行自动化脚本，免去手动盯屏。
- **每轮流程**：回到主界面（`game_main`）→ 进主城 → 按 ROI 巡视角逐一确认征兵详情 → 对符合条件的队伍执行预设的编队 / 集结类模板点击 → 结束后返回主城界面。
- **Dear ImGui 界面**：ADB、模板路径、阈值、`scheduler`、`team_rois`、`game.`* 等可在界面调整并保存为 JSON。

## 依赖

- **OpenCV**（建议 Homebrew：`brew install opencv`）。若 CMake 找不到 OpenCV，可指定：`cmake .. -DOpenCV_DIR=/opt/homebrew/lib/cmake/opencv4`。
- **GLFW、Dear ImGui、nlohmann/json** 由 CMake `FetchContent` 自动拉取。

