"""The operator: device actions, and the handoff into the scripter.

The host sends one flat tool list. Filtering happens here, so C++ never learns
that two agents exist.
"""

from __future__ import annotations

from typing import Annotated

from langchain_core.tools import tool
from pydantic import Field

import agent.scripter as scripter
import models
from agent.region_rules import REGION_RULES

# Not in the C++ registry. `run_tools` intercepts this name and runs the
# scripter; this body is not called. Defaults keep every argument but `goal`
# out of the schema's `required`.
@tool
def author_script(
    goal: Annotated[str, Field(description=(
        "这个脚本要做什么。用用户的原话，再加上你从屏幕上看到的信息。"))],
    steps: Annotated[str, Field(description=(
        "你已经在设备上做过的事，一步一行：当时停在哪个界面，点过哪个控件，"
        "该控件最终的区域是什么，那个区域里应该有什么。写脚本的 agent 看不到"
        "这段对话。某次 tap 的结果里写着 `已经改用` 时，抄那个区域，"
        "不要抄你原本提议的那个。"))] = "",
    bundle: Annotated[str, Field(description=(
        "要新建或修改的 bundle；留空让写脚本的 agent 自己选。"))] = "",
    shots: Annotated[list[int], Field(description=(
        "能说明涉及到的界面的截图编号。"))] = [],
    last_error: Annotated[str, Field(description=(
        "上一次 run_script 的报错原文，修故障时用。"))] = "",
    fix: Annotated[bool, Field(description=(
        "仅当你确认用户要改的 bundle 已经存在时为 true。新脚本、新界面、"
        "新的边都不要填。"))] = False,
) -> str:
    """把写、修、管理 .ccat bundle 的活交给写脚本的 agent，包括给某个游戏的
    图添加界面和移动。它能读写 bundle、切模板、跑脚本来验证，然后回报。
    把用户说过的每一条约束原样放进 `goal`：写脚本的 agent 看不到这段对话的
    其他内容。没有 `fix: true` 也没有 `last_error` 时，它改不动已经存在的
    bundle。`steps` 是你自己在设备上已经做过的事：见过的界面，以及每个点过
    的控件最终用的区域。少了它，写脚本的 agent 会把每个控件重新找一遍。
    """
    raise RuntimeError("author_script is dispatched by run_tools")


AUTHOR_SCRIPT_NAME = author_script.name

# The operator drives the device and hands bundle work off. It does not see
# `read_script` or the language reference; those belong to the scripter.
OPERATOR_PERSONA = (
    "你用工具操作一台 Android 设备。收到操作设备的请求，先调 `list_scripts`；"
    "已经存好的技能能干这件事，就用 `run(\"<name>/main.ccat\")` 跑它，"
    "不要自己重做一遍步骤。`@name` 就是那个技能——直接跑，不要去搜。"
    "写、改、修 bundle 都是 `author_script`：把用户说过的每一条约束原样放进 "
    "`goal`。只有当用户要改一个已经存在的 bundle 时才传 `fix: true`，"
    "上一次 `run_script` 失败时传 `last_error`。其余任何交接都不许改动已存在"
    "的 bundle。"
    "把你查看过的每个界面、点过的每个控件写进 `steps`，一行一个，"
    "带上最终区域和该区域里应该出现的内容；结果里给出 `已经改用` 的区域"
    "时，抄那一个。"
    "带 `[graph <graph>, ...]` 标签的游戏有一张界面地图。`goto` 之前先调 "
    "`read_graph` 并传入那个图名：它列出所有 anchor、所有边和所有 action。"
    "要到其中某个 anchor，用 `run_script` 传入 "
    "`goto(\"<graph>:<anchor>\")`，anchor id 用那张列表里的。"
    "一件事做完就立刻交接，不要等整轮结束。"
    "`locate(\"<graph>\")` 回答 `unknown` 的界面就是一个新 anchor：当场 "
    "`author_script`，goal 里说清这个界面是什么、你是从哪个 anchor 来的"
    "（或者说明哪个都不是），并且在这之前不要在这里再点任何东西——"
    "连着开两个新 anchor，这条边就没有起点了。只有在还没有任何一次运行报出过"
    "这个界面时才用 `locate`；`ambiguous` 是图本身的故障，不是新界面：报告它。"
    "一个图里装的第三样东西是 action：花掉资源或领取资源的那一下点击——"
    "收菜、签到、领奖——存成 `<graph>.<anchor>.<action>`。用 `author_script` "
    "交接，`bundle` 填这个名字，任务作为 goal，这个界面上的点击写进 `steps`。"
    "只是状态变了（按钮置灰、数字变了）时，界面还是同一个；action 中途可以离开"
    "这个页面——说清它是从哪个 anchor 开始的。那一下点击做完并核对完就交接，"
    "在切到下一个标签之前。做了任务之后 `read_graph` 仍显示 `actions: 0`，"
    "说明这个图不是用户要的那个。"
    "先截屏（否则你看不到屏幕），看清结果再走下一步。`tap` 指定的区域，"
    "用的就是那张截图的像素。\n" + REGION_RULES +
    "必须从已知界面开始的流程，以 `go_home` 开头：它回到桌面并关掉所有最近"
    "打开的应用。模拟器没启动时截屏会失败并说明原因。`start_emulator` 会启动它"
    "并一直阻塞到实例就绪，所以这一条要单独调用：同一轮里的截屏会先执行，"
    "那时还在启动，也会失败。"
    "用户只是提问时，回答问题就行。"
)

# Said instead of `OPERATOR_PERSONA` when the host records training samples: it
# names the two tools that exist, and what a region is for, so a redraw is a
# correction the model can learn from rather than a guess at a coordinate.
SOLO_PERSONA = (
    "你只用四个工具操作一台 Android 设备：`screenshot`、`tap`、`go_home` 和 "
    "`start_emulator`。你不能跑已保存的脚本，也不能写脚本；不要提出或尝试这类"
    "操作。先截屏（否则你看不到屏幕），每一次 tap 之前都要看过截图。"
    "`tap` 只指定一个区域——方块 `{x,y,w,h}`、圆 `{cx,cy,r}` 或菱形 "
    "`{cx,cy,rx,ry}`，全部用你正在看的那张截图的像素——点击落在区域中心。\n"
    + REGION_RULES +
    "压到背景上的框就是用户要重画的。用户重画了你的区域时，以他画的为准："
    "照它点，下次也照它的样子画。改变了界面的操作之后要重新截屏，不要自己"
    "猜结果。必须从已知界面开始的流程，以 `go_home` 开头：它回到桌面并关掉"
    "所有最近打开的应用。模拟器没启动时截屏会失败并说明原因；"
    "`start_emulator` 会启动它并一直阻塞到实例就绪，所以这一条要单独调用——"
    "同一轮里的截屏会先执行，那时还在启动，也会失败。用户只是提问时，"
    "回答问题就行。"
)

_RULES = {
    "自主": (
        "直接动手，不要反问。该走下一步就走，不要停在一个你自己动手就能解决的"
        "问题上。对设备或磁盘的改动本来就要等审批，先用文字问一遍等于问两次；"
        "猜错了只是被驳回一次，不是犯错。"
    ),
    "格式": (
        "用中文回复。渲染器只认这几种写法：行首的 '#'/'##'/'###' 加一个空格；"
        "'**粗体**'，且开头的标记前面要有一个空格；列表项写成 '  * '"
        "（两个空格、星号、空格），每深一层再缩进两个空格；单独一行的 '***' "
        "当分隔线。表格、代码围栏、有序列表、'-' 开头的列表、图片，以及单个 "
        "'*' 的斜体，都会把标记原样显示出来。\n"
        "\n"
        "## 当前界面\n"
        "我看到的是 **微信** 主界面，选中的是聊天标签页。\n"
        "\n"
        "  * 顶部有一个搜索框\n"
        "    * 第一行有未读\n"
        "  * 点它。**然后等。**\n"
        "\n"
        "***\n"
        "正在打开聊天标签页。"
    ),
}

_RULE_NAMES = ("自主", "格式")

# The tools `solo` leaves: look at the screen, act on a region, boot a device
# that is down, and get back to a known screen. Named here rather than derived,
# because this list is what a training sample may contain.
SOLO = frozenset({"screenshot", "tap", "start_emulator", "go_home"})


def _rules(names: tuple[str, ...]) -> str:
    """One `## name` block per rule."""
    return "\n\n".join(f"## {name}\n\n{_RULES[name]}" for name in names)


def _name(tool: dict) -> str | None:
    try:
        name = tool["function"]["name"]
    except (KeyError, TypeError):
        return None
    return name if isinstance(name, str) else None


def operator_tools(host_tools: list, solo: bool = False) -> list:
    """Host tools the operator may call, plus the handoff tool.

    `solo` is what the host asks for while it records training samples: the
    operator drives alone, and with only the tools a sample needs -- look at the
    screen, then act on a region. Everything else is withheld, including the
    handoff, so one agent's regions are all a sample can contain.
    """
    hidden = scripter.SCRIPTER_ONLY | scripter.HIDDEN
    kept = [tool for tool in host_tools if _name(tool) not in hidden]
    if solo:
        return [tool for tool in kept if _name(tool) in SOLO]
    return kept + [author_script]


def operator_prompt(solo: bool = False) -> str:
    """Who the operator is. An env override replaces the whole thing, cheatsheet included."""
    override = models._env("CAMPCAT_LLM_SYSTEM_PROMPT")
    if override:
        return override
    persona = SOLO_PERSONA if solo else OPERATOR_PERSONA
    return persona + "\n\n" + _rules(_RULE_NAMES)
