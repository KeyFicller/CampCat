# CCAT for VSCode

`.ccat` 脚本的语法高亮和 snippets。纯声明式扩展：只有 JSON 和这份 README，没有运行时代码。

## 本地跑起来（改这个扩展时用）

1. VSCode 打开**这个目录**（`editors/vscode/`，不是仓库根目录）。
2. 按 `F5`，会弹出一个扩展开发宿主窗口。
3. 在新窗口里打开任意 `.ccat` 文件。

## 装到日常用的 VSCode

打包出一个 `.vsix`，再手动装：

```bash
cd editors/vscode
npx @vscode/vsce package
code --install-extension ccat-0.1.0.vsix
```

## 规则与内置编辑器一致

高亮的词法规则来自 `src/core/script/ccat_lexer.cpp`，配色分类对齐 `src/app/ui/ccat_highlight.cpp`。
本目录的 `syntaxes/ccat.tmLanguage.json` 是它的**第三份拷贝**（另两份：lexer、内置高亮）。

**改 `.ccat` 语法时，三处都要动**：

| 位置 | 改什么 |
|---|---|
| `src/core/script/ccat_lexer.cpp` | 真词法：新增关键字就在这里 |
| `src/app/ui/ccat_highlight.cpp` | 内置编辑器的关键字表（有 `ccat_highlight_test` 兜住与 lexer 的漂移） |
| `syntaxes/ccat.tmLanguage.json` | 本扩展的关键字表（**没有测试兜底**） |

前两处的漂移会红测试，第三处不会 —— 这是本扩展已知的维护代价。

## 关键字边界为什么不能用 `\b`

CCAT 的标识符把 `.` 和 `-` 算进词里，所以 `tap.png` 的 `tap`、`close-x` 的 `close` 都不是关键字。
规则里的边界一律写 `(?<![\w.-])` / `(?![\w.-])`，写成 `\b` 会把它们全误判成关键字。

## 不做的事

诊断、补全、跳转、hover 都没有 —— 那需要 LSP 或复用 C++ 解析器，不在本扩展范围内。
高亮纯装饰：规则与解析器有出入也不会影响脚本能否运行。
