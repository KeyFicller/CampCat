# CampCat

Mac 上跑 Android 模拟器的挂机壳子：ADB 截图 + OpenCV 模板匹配，用 `.ccat` 脚本驱动点击与等待。

## 能干什么

- **脚本自动化**：流程写在 `config/scripts/*.ccat`；`if` / `tap` / `tap_offset` / `swipe` / `wait` / `loop` / `retry` / `home` / `run("…")` 等。
- **定时调度**：间隔 + 抖动，周期跑当前脚本。
- **ImGui 壳**：Run / Script / Settings + 底部常驻 Log；调 ADB、匹配参数、脚本路径；截图裁 ROI 存成与脚本同目录的 PNG。

## 构建

依赖：Homebrew OpenCV；GLFW / ImGui / nlohmann/json 由 CMake 拉取。

```bash
cmake -S . -B build -DOpenCV_DIR=/opt/homebrew/lib/cmake/opencv4
cmake --build build --target campcat
./build/campcat
```

离线语法冒烟：`ctest --test-dir build -R ccat_syntax`
