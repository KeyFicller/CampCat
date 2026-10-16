# CampCat

**手机模拟器 Agent 壳** —— 初衷很朴素：手游的日常任务，人不用守着。

## 两条路

![intro](campcat/sources/cpp/assets/images/intro.jpg)

**脚本驱动** —— 为了确定性，定时调度的时候不漂移。OpenCV 在本地匹配模板，没有大模型参与：同一个屏幕，每次都是同一串点击，稳定，可复现。技能包是一段 `.ccat` 脚本加上它的模板图，语言见 [CCAT.md](ccat_library/CCAT.md)。

**LLM 探索** —— 为了把写脚本的迭代成本压下来。人只说一句目标，视觉模型看屏幕去探索，跑通的经验写成脚本落盘，再交给上面那条路，本质上是个脚本作者。

## LLM 探索工作流

![workflow](campcat/sources/cpp/assets/images/workflow.jpg)


| Agent         | 何时上场      | 做什么                              |
| ------------- | --------- | -------------------------------- |
| operator      | 和人对话      | 看屏幕、操作设备、跑已有的技能包。写脚本交给 scripter。 |
| scripter      | 被叫来写脚本    | 只看见这次的目标。建图、切模板、跑一遍验证。           |
| diagnoser     | 模板没匹配上    | 看模板和屏幕，告诉 scripter 是重切，还是改上一步。   |
| region review | 按一块区域动手之前 | 对照裁出来的图改到合适。看不清就用原来的。            |



### 边走边写
![exploring](campcat/sources/cpp/assets/images/exploring.png)

## 图的三件套

![memory](campcat/sources/cpp/assets/images/memory.jpg)


| 落盘图内的元素     | 做什么                   |
| ----------- | --------------------- |
| 锚点（主要认UI前景） | 用这一屏上的模板，判断现在是不是这页。   |
| 边           | 从这一页走到那一页。            |
| 动作          | 从这一页开始把事情做完，结束时还是这一页。 |

### 写成的图
![graph_built](campcat/sources/cpp/assets/images/graph_built.png)