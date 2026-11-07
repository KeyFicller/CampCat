"""The operator while the host records training samples.

One agent, and only the tools a sample may contain: look at the screen, act on
a region, boot a device that is down, get back to a known screen. No handoff
and no saved script, so a second agent never lands in the sample.
"""

from __future__ import annotations

import agent.operator as operator
import models
from agent.region_rules import REGION_RULES


# Named here rather than derived: this list is what a training sample may contain.
SOLO = frozenset({"screenshot", "tap", "start_emulator", "go_home"})

# Said instead of the operator persona. It names the tools that exist, and what
# a region is for, so a redraw is a correction the model can learn from rather
# than a guess at a coordinate.
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


def _name(tool: dict) -> str | None:
    try:
        name = tool["function"]["name"]
    except (KeyError, TypeError):
        return None
    return name if isinstance(name, str) else None


def tools(host_tools: list) -> list:
    """The host tools a sample may call, in the host's order."""
    return [tool for tool in host_tools if _name(tool) in SOLO]


def prompt() -> str:
    """Who drives a sample. The same env override as the operator replaces it."""
    override = models._env("CAMPCAT_LLM_SYSTEM_PROMPT")
    if override:
        return override
    return SOLO_PERSONA + "\n\n" + operator.shared_rules()
