# CampCat

Mac 上跑 Android 模拟器的挂机壳子：ADB 截图 + OpenCV 模板匹配，用 `.ccat` 脚本驱动点击与等待；另有一条 LLM 路径，把截图交给 `deepseek-flash` 出文字描述。

## 最终目标

**人会挂机。** 人只说一句目标（"把这个刷完"），模型自己看屏幕、自己写并运行 `.ccat` 脚本、看结果、错了自己改。

走到头的形态：模型写完的脚本**落盘、版本化**，成为可复用的技能库；`写 → 跑 → 看结果 → 改` 这个环能反复走，不必每轮从零写起。

底子不变：ADB + OpenCV 模板匹配 + `.ccat` 仍是唯一的执行层。LLM 只是站到了"脚本作者"的位置上——它不绕过解释器，也不自己戳屏幕。

## 能干什么

- **脚本自动化**：`.ccat` 脚本放 `config/` 下（一般 `config/scripts/`），模板 PNG 放**同一目录**；语法见 [CCAT.md](CCAT.md)。
  `config/scripts/<id>.json` 的 `source` 指向脚本（相对 `config/`）；`config/default.json` 的 `scripts` 登记有哪些 id、`active_script` 选当前跑哪个。
- **定时调度**：间隔 + 抖动，周期跑当前脚本。
- **LLM**：把截图（模拟器 ADB 截图或本地图片）发给 OpenAI 兼容的 `deepseek-flash`，拿回文字描述；API key 优先读环境变量 `DEEPSEEK_API_KEY`，没有则读 `llm/.env`（dotenv 格式，已 gitignore）。
- **Console**：ImGui 内交互输入 `.ccat` 片段（`>>>` / `...` 多行），真机执行；与 cycle 互斥。
- **ImGui 壳**：Script / LLM / Console / Settings + 底部常驻 Log；调 ADB、匹配参数、脚本路径；截图裁 ROI 存成与脚本同目录的 PNG。

## 构建

依赖：Homebrew OpenCV；GLFW / ImGui / nlohmann/json / portable-file-dialogs 由 CMake 拉取。

```bash
cmake -S . -B build -DOpenCV_DIR=/opt/homebrew/lib/cmake/opencv4
cmake --build build --target campcat
./build/campcat
```

离线语法冒烟：`ctest --test-dir build -R ccat_syntax`
