"""The operator: device actions, and the handoff into the scripter.

The host sends one flat tool list. Filtering happens here, so C++ never learns
that two agents exist.
"""

from __future__ import annotations

from typing import Annotated

from langchain_core.tools import tool
from pydantic import BaseModel, Field

import agent.scripter as scripter
import models
from agent.region_rules import REGION_RULES


# Fields rather than a paragraph: the free-text version carried three region
# spellings in one handoff (`{x,y,w,h}`, `box x,y wxh`, and a bare centre point)
# and put two regions on one line, so the scripter had to guess which one landed.
# No class docstring on purpose -- pydantic copies it into the schema, and it is
# sent to the model on every request.
class HandoffStep(BaseModel):
    shot: Annotated[int, Field(description=(
        "当时停在哪个界面：那一帧的编号。一步只写一帧；看过好几帧就拆成好几步。"))]
    screen: Annotated[str, Field(description=(
        "这个界面的名字：标题文字，或者你给它的叫法。"))]
    did: Annotated[str, Field(default="", description=(
        "你在这个界面上做了什么：点了哪个控件、滑了哪里。只是看了一眼就写「观察」。"))] = ""
    region: Annotated[str, Field(default="", description=(
        "那个控件的最终区域，照工具结果的写法原样抄：`box x,y wxh`、"
        "`circle cx,cy,r` 或 `diamond cx,cy,rx,ry`。没有点击就留空。"
        "工具结果里写着 `已经改用` 时抄那一个，不要抄你提议的。"))] = ""
    expect: Annotated[str, Field(default="", description=(
        "这个区域里应该有什么，一句。"))] = ""
    result: Annotated[str, Field(default="", description=(
        "做完之后看到什么：哪个数字变了、哪个按钮变灰、弹出了什么。"))] = ""


# Not in the C++ registry. `run_tools` intercepts this name and runs the
# scripter; this body is not called. Defaults keep every argument but `goal`
# out of the schema's `required`.
@tool
def author_script(
    goal: Annotated[str, Field(description=(
        "分两段。第一段只说这一次要产出的那一个东西——一个锚点，或者一个动作，"
        "用祈使句，用自己的话说这一屏。第二段以「上下文（不是这次要做的）」开头，"
        "放用户的整体要求和他说的每一条约束：写脚本的 agent 看不到这段对话，"
        "少了它就不知道边界。整张图的目标写进第一段，一次交接就会把整张图做完。"))],
    steps: Annotated[list[HandoffStep], Field(description=(
        "你已经在设备上做过的事，一个控件一项。写脚本的 agent 看不到这段对话。"))] = [],
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
    把用户说过的每一条约束原样放进 `goal` 的第二段：写脚本的 agent 看不到这段
    对话的其他内容，第一段只留这一次要产出的那一个东西。没有 `fix: true`
    也没有 `last_error` 时，它改不动已经存在的 bundle。`steps` 是你自己在设备
    上已经做过的事：一个控件一项——见过的界面、每点过的控件、它的最终区域和
    那里应该出现的东西。少了它，写脚本的 agent 会把每个控件重新找一遍。
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
    "`goal`。要改一个已经存在的 bundle 时必须传 `fix: true`——包括你自己这一轮"
    "刚建好、但判据不对的锚点（比如标题模板在别的屏也命中），不传就会被拒，"
    "同一个改法重交几次也不会成；一次只修这一处。"
    "上一次 `run_script` 失败时传 `last_error`。其余任何交接都不许改动已存在"
    "的 bundle。"
    "把你查看过的每个界面、点过的每个控件写进 `steps`，一个控件一项；区域照抄"
    "工具结果里的写法（`box x,y wxh` / `circle cx,cy,r` / `diamond cx,cy,rx,ry`），"
    "结果里给出 `已经改用` 的区域时，抄那一个。"
    "带 `[graph <graph>, ...]` 标签的游戏有一张界面地图。`goto` 之前先调 "
    "`read_graph` 并传入那个图名：它列出所有 anchor、所有边和所有 action。"
    "要到其中某个 anchor，用 `run_script` 传入 "
    "`goto(\"<graph>:<anchor>\")`，anchor id 用那张列表里的。"
    "交接的时机是「一件事刚做完」，不是「整轮走完」：你同时只留得住最新几张画面，"
    "晚交就得凭记忆补草图和框，写脚本的 agent 还要把你走过的屏重新找一遍。"
    "到一屏先认清它是哪个 anchor：刚 `goto` 过去、或者上一步的结果已经报了 anchor，"
    "就不用再查；不知道才 `locate(\"<graph>\")` 查一次。"
    "回答 `unknown` 就是新 anchor：当场 `author_script`，goal 里写清这一屏是什么、"
    "从哪个 anchor 过来（或者说明它是起始屏），`shots` 给当前这张。在这之前"
    "不要再点任何东西，也不要先走到下一屏——连着开两个新 anchor，那条边就没有起点了。"
    "回答某个 anchor 就不是新屏，不交；`ambiguous` 是图本身的故障，不是新界面："
    "报告它，不要为它交接。"
    "一项任务做完、并且成功过一次，就交接：任务是在同一屏上开始、也在这一屏收尾的"
    "那件事，goal 写这件事加上用户的原话约束，`steps` 写这屏点过的每个控件。"
    "只回答一个问题、跑一个已保存的技能、或者跨了屏还没回到起始屏，都不算交接的时机。"
    "暂不交跨屏的任务：一条流程经过多屏也要回到起始屏收尾，动作仍挂在起始屏上。"
    "不要攒着一起交：跨多屏的流程按「发现新屏→交锚点和那条边→在这屏把事做完→交动作」"
    "一步一交。一次交接只产出一个东西——一个锚点（连带它的一条边），或者一个动作。"
    "把要建的东西拆开交，写脚本的 agent 每次只面对一件小事，比你交一大堆做得快。"
    "`goal` 的第一段只说这一次要产出的那一个东西，用户的整体要求（「把整张图建完整」"
    "这类）放进第二段，以「上下文（不是这次要做的）」开头——写进第一段，写脚本"
    "的 agent 就会在一次交接里把整张图都做完，你再没有机会把它拆开。"
    "一个图里装的第三样东西是 action：花掉资源或领取资源的那一下点击——"
    "收菜、签到、领奖——存成 `<graph>.<anchor>.<action>`。用 `author_script` "
    "交接，`bundle` 填这个名字，任务作为 goal，这个界面上的点击在 `steps` 里各写一项。"
    "action 那一段是英文小写（`harvest`，不是 `收菜`）：名字落地之后只有改名"
    "能补救，而改名要再来一轮。"
    "只是状态变了（按钮置灰、数字变了）时，界面还是同一个，不新开 anchor；"
    "做完一下资源操作就当场交接，别等切走之后凭记忆写——一条流程中途跑去了别的屏，"
    "也先回到起始屏再交，动作挂在起始屏上。做了任务之后 `read_graph` 仍显示 `actions: 0`，"
    "说明这个图不是用户要的那个。"
    "先截屏（否则你看不到屏幕），看清结果再走下一步。`tap` 指定的区域，"
    "用的就是那张截图的像素。每张图的说明都写了它「按 WxH 显示，读到的数 ×k」："
    "在哪张图上量的就乘那张的 k，不要拿另一张的尺度。说明里写着`图片未附上`"
    "的帧是你确实看过的：要看就 `view_shot(<编号>)`，不要 `go_home` 重走一遍。"
    "只有最新几张带图，每看一张会挤掉另一张——要回顾就挑一张，别顺着编号一路看下去。\n" + REGION_RULES +
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
