Mac（及部分 POSIX）下、面向 **Android 模拟器** 的挂机小助手：通过 **ADB 截图 + OpenCV 模板匹配**，由 **CampCat（`.ccat`）脚本** 驱动自动化点击与等待逻辑。

## 功能概要

- **脚本驱动**：仅保留 `ccat_script`；流程写在 `config/scripts/` 下的 `.ccat`，由 `ccat_script.json` 指定源文件与模板目录。
- **定时调度**：可配置间隔与可选抖动，周期性执行当前脚本。
- **Dear ImGui**：ADB、匹配阈值、`scheduler`、脚本路径可调；提供「ADB 截图裁剪」将 ROI 存为模板 PNG（写入脚本 `images_base`）。

语句含 `if (模板.png)`、`tap(...)`、`tap_at(x,y)`、`wait(毫秒)`、`wait_until(...)`、`loop` / `do…while`、`break`、`retry`、`log(...)` 及 `{ ... }` 块等。

## 依赖

- **OpenCV**（建议 Homebrew：`brew install opencv`）。若 CMake 找不到 OpenCV，可指定：`cmake .. -DOpenCV_DIR=/opt/homebrew/lib/cmake/opencv4`。
- **GLFW、Dear ImGui、nlohmann/json** 由 CMake `FetchContent` 自动拉取。
