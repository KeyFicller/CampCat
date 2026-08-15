Mac（及部分 POSIX）下、面向 **Android 模拟器** 的挂机小助手：通过 **ADB 截图 + OpenCV 模板匹配**，由 **CampCat（`.ccat`）脚本** 驱动自动化点击与等待逻辑。

## 功能概要

- **脚本驱动**：仅保留 `ccat_script`；流程写在 `config/scripts/` 下的 `.ccat`，由 `ccat_script.json` 指定源文件与模板目录。
- **定时调度**：可配置间隔与可选抖动，周期性执行当前脚本。
- **Dear ImGui**：ADB、匹配阈值、`scheduler`、`.ccat` 路径可调；「ADB 截图裁剪」把 ROI 存为与脚本同目录的 PNG。未指定脚本路径时禁止 Save。

语句含可选顶栏 `defs { name = 字面量 }`（数字 / 字符串 / 布尔，仅在解析该脚本时展开）、`if (模板.png)`、`tap(...)`、`tap_at(x,y)`、`tap_offset(图, dx, dy)`（中心 + 整屏归一化偏移）、`wait(毫秒)`、`wait_until(...)`、`loop` / `do…while`、`break`、`return`、`home`（回桌面并对近期任务 `am force-stop`）、`run("其它.ccat")`（相对当前脚本目录，可用 `..`）、`retry`、`log(...)` 及 `{ ... }` 块等。

## 依赖

- **OpenCV**（建议 Homebrew：`brew install opencv`）。若 CMake 找不到 OpenCV，可指定：`cmake .. -DOpenCV_DIR=/opt/homebrew/lib/cmake/opencv4`。
- **GLFW、Dear ImGui、nlohmann/json** 由 CMake `FetchContent` 自动拉取。
