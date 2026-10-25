"""The scripter: bundle writing, and the handoff subgraph.

Imported by `graph.py`. This module does not import `graph` at load time;
the subgraph nodes do, once `graph` has finished loading.
"""

from __future__ import annotations

import contextvars
from pathlib import Path

from langchain_core.messages import HumanMessage, SystemMessage, ToolMessage
from langgraph.graph import END, StateGraph

import agent.diagnoser as diagnoser
import agent.memorier as memorier
import observability
import session
import subagent
import tools
from agent.region_rules import REGION_RULES


# Only the scripter sees these. Anything the host sends that is not listed here
# or in HIDDEN goes to the operator, so a new C++ tool lands with today's behaviour.
SCRIPTER_ONLY = frozenset({
    "read_script", "ccat_help",
    "save_script", "update_script", "delete_script", "rename_script",
    "save_template", "view_template", "view_crop",
    "save_anchor",
})
# Registered in C++ for the review pipeline, which calls them itself. A model
# is not offered them: the approval bubble already shows a region.
HIDDEN = frozenset({"mark", "tight_crop"})
# Offered to both: the operator runs saved skills, looks at the screen and
# reads a graph before goto; the scripter verifies what it wrote and cuts
# templates from fresh frames.
SHARED = frozenset({
    "list_scripts", "run_script", "screenshot", "view_shot", "read_graph",
})


def _name(tool: dict) -> str | None:
    try:
        name = tool["function"]["name"]
    except (KeyError, TypeError):
        return None
    return name if isinstance(name, str) else None


def scripter_tools(host_tools: list) -> list:
    """Host tools the scripter may call, in the host's order, then diagnose_template."""
    offered = SCRIPTER_ONLY | SHARED
    return [tool for tool in host_tools if _name(tool) in offered] + [
        diagnoser.DIAGNOSE_TEMPLATE]


def brief_of(args: dict) -> str:
    """The scripter's one user message. Empty optional fields are left out."""
    lines = [f"goal: {args.get('goal', '')}"]
    steps = (args.get("steps") or "").strip()
    if steps:
        lines.append(f"steps:\n{steps}")
    bundle = args.get("bundle") or ""
    if bundle:
        lines.append(f"bundle: {bundle}")
    shots = args.get("shots")
    if isinstance(shots, list) and shots:
        lines.append("shots: " + " ".join(str(shot) for shot in shots))
    if args.get("fix") is True:
        lines.append("fix: true")
    last_error = args.get("last_error") or ""
    if last_error:
        lines.append(f"last_error: {last_error}")
    return "\n".join(lines) + session.ASK_IN_CHINESE


def handoff_may_change(args: dict) -> bool:
    """True when this handoff may edit bundles that already exist."""
    if str(args.get("last_error") or "").strip():
        return True
    return args.get("fix") is True


def bundle_names_of(listing: str) -> set[str]:
    """Bundle names from a `list_scripts` reply. `none yet` is an empty set."""
    if listing.strip() == "none yet":
        return set()
    names = set()
    for line in listing.splitlines():
        name = line.split(" -- ", 1)[0].strip()
        if name:
            names.add(name)
    return names


# The field that names the bundle a write would change. `save_anchor` is
# handled beside this: its name is `graph.anchor`.
_FROZEN_FIELD = {
    "update_script": "name",
    "delete_script": "name",
    "rename_script": "name",
    "save_template": "bundle",
}


def refuse_existing(tool: str, args: dict, frozen: set[str]) -> str | None:
    """The refusal the model sees, or None when the call may reach the host."""
    if tool == "save_anchor":
        graph_name = args.get("graph")
        anchor = args.get("anchor")
        if (not isinstance(graph_name, str) or not isinstance(anchor, str)
                or not graph_name or not anchor):
            return None
        target = graph_name + "." + anchor
    else:
        field = _FROZEN_FIELD.get(tool)
        if field is None:
            return None
        target = args.get(field)
        if not isinstance(target, str) or not target:
            return None
    if target not in frozen:
        return None
    return f"error: {target} 已存在；这次交接不能改它"


# None means this handoff may change existing bundles. A set is the names
# that existed when it started.
_frozen: contextvars.ContextVar[frozenset[str] | None] = contextvars.ContextVar(
    "scripter_frozen", default=None)


def arm_freeze(names: frozenset[str] | None):
    """Remember `names` until `disarm_freeze`. None leaves existing bundles writable."""
    return _frozen.set(names)


def disarm_freeze(token) -> None:
    _frozen.reset(token)

# How a reply is written, one entry per part of the rule. Fed as Markdown with
# the key as a heading, so a part can be rewritten or dropped on its own.
RULES = {
    "脚本流程": (
        "写或改一个脚本，就是这个顺序，一步都不跳过：\n"
        "\n"
        "1. 先 `list_scripts`。已经有 bundle 能干这件事，就用 "
        "`run(\"<name>/main.ccat\")` 跑它。只有当 brief 里有 `fix: true` 或 "
        "`last_error` 时才改已存在的 bundle。否则不要对它 "
        "`update_script`、`delete_script`、`rename_script` 或 `save_template`，"
        "也不要在已存在的 anchor 上 `save_anchor`；写一个新名字的新 bundle。"
        "有图的游戏上的任务，先 `goto(\"<graph>:<anchor>\")` 去到它工作所在的"
        "界面；图里还没有的界面要先加（见「图的流程」）。\n"
        "2. `screenshot`，这样你能看到自己要写脚本的那个界面。\n"
        "3. 给每一步定 anchor：只有这一步成功才会出现的图，取自固定的 UI 控件"
        "——一个按钮、一个标签页、一个标签——绝不用自由文本，它每次运行都会变，"
        "会让检查失去意义。把那一步期望看到的东西写成 `//` 注释放在旁边"
        "（`wait_until(\"claim_taken.png\", 8000)"
        "   // 领完之后按钮会变灰`）：这样失败的一次运行就有期望可以对照，"
        "审批时也读得懂。\n"
        "4. `save_script` 创建 bundle。它只写 main.ccat，不运行任何东西；"
        "名字已存在会被拒绝。\n"
        "5. `save_template`，脚本里出现的每一个 PNG 各调一次。每次裁之前先截屏"
        "——裁得再好也不会好过它来源的那张图——围着不动的部分裁紧。"
        "对那个元素只调一次 `view_crop`，然后 `save_template`。只是为了把同一个框"
        "再收紧一点而重复调用，不会得到更好的裁切。形状由你根据控件外轮廓自己"
        "挑。矩形是方块 `{\"x\":...,\"y\":...,\"w\":...,\"h\":...}`。"
        "圆形控件是圆 `{\"cx\":...,\"cy\":...,\"r\":...}`。菱形"
        "（菱形，顶点在坐标轴上）是 `{\"cx\":...,\"cy\":...,\"rx\":...,\"ry\":...}`。"
        "圆形或菱形控件外面套方块，会把匹配必须忽略的背景留下。圆或菱形会写出"
        "该形状的 alpha，形状之外的像素一律不参与匹配。\n" +
        REGION_RULES +
        "6. 跑一遍 bundle：`run_script`，源码传 `run(\"<name>/main.ccat\")`。"
        "传这个 `run(...)` 调用，不要传脚本正文——光秃秃的 `run_script` 会相对"
        "脚本根目录解析模板名，你刚存进 bundle 的那些 PNG 它看不见。\n"
        "7. 用 `update_script(name, source, templates)` 改已有的东西：源码和"
        "整套模板一次交付，一次审批。每一项是 "
        "`{\"name\":...,\"cut\":{\"x\":...,\"y\":...,\"w\":...,\"h\":...},\"shot\":...}`"
        "（方块）、`{\"cx\":...,\"cy\":...,\"r\":...}`（圆），或者 "
        "`{\"cx\":...,\"cy\":...,\"rx\":...,\"ry\":...}`（菱形）。重新裁时用"
        "符合控件的那个形状，不要沿用旧模板碰巧用的形状。`shot` 省略表示最近"
        "一张截图。`update_script` 只用于这次交接自己创建的 bundle，或者 brief "
        "里用 `fix: true` / `last_error` 放行的那个。`save_script` 会拒绝已存在的"
        "名字。"
    ),
    "脚本规则": (
        "  * `view_crop` 的图会画出裁切，并显示框外最多 16 像素的屏幕。线以内"
        "就是 `save_template` 保存的内容。线以外是上下文：不要为了把它纳进来而"
        "放大区域，也不要为了把它排除掉而重新裁。\n"
        "  * 某条语句失败会中止脚本。回到你手上的就是这个报错，而不是一个看起来"
        "不对的屏幕。\n"
        "  * 给每一个动作配 anchor。tap、swipe 或 `run(...)` 之后，用 "
        "`wait_until(\"expected.png\", ms)` 断言它本该产生的界面。没落地的动作"
        "会当场在那里失败并指名文件，而不是让下一条语句在一个过时的界面上执行。"
        "anchor 的模板要随脚本一起保存。模板路径就是这个 bundle 里的文件名；"
        "带 `/` 或 `..` 的会被拒绝，所以在这里裁图，不要从别的 bundle 借。"
        "复用技能仍然用 `run(\"../other/main.ccat\")`。\n"
        "  * 每一条 `log(...)` 都要有 anchor 依据：绝不要单独打印「任务完成」。"
        "声明某个状态的 `log(...)`，就放在确认该状态的那个模板的 `wait_until` 或 "
        "`if` 之后。\n"
        "  * `ms` 要从截图里量出来，不要猜。每张截图的说明都写了它是什么时候拍的，"
        "同一轮之内还写了距上一张过了多久；花了 +3.2s 的变化就给大约两倍。"
        "`wait_until` 一看到 anchor 就返回，所以给宽一点不花代价。两个等待都有上限，"
        "是截断而不是报错——`wait(ms)` 截在 5000 ms，`wait_until(\"a.png\", ms)` "
        "截在 20000 ms——所以写大了会静默变成上限，你拿到的时间比要的少；"
        "`wait_until(\"a.png\", 0)` 是语法错误。超时是一个会中止脚本的错误。\n"
        "  * 已存在的 bundle 是拿来跑的，不是拿来改的，除非 brief 里有 "
        "`fix: true` 或 `last_error`。`save_script` 仍然拒绝已存在的名字。"
        "这次交接里创建的 bundle 可以更新。\n"
    ),
    "失败处理": (
        "脚本失败时停在失败的那条语句上，报错会带着匹配器看到的东西回到你这里"
        "（`best=`、它偏好的 `box`、`threshold`）。那个 `threshold` 是这次匹配"
        "被衡量的门槛：圆形或菱形模板是通过 alpha 遮罩匹配的，可用的信息比方形"
        "模板少，所以两者的门槛不同。先弄清是哪一边错了，再动手改：\n"
        "\n"
        "0. 报错里带 `best=` 和 `box=` 时，可以先调 "
        "`diagnose_template(bundle, template, error, expect)`，报错原样传，"
        "那一步的 `//` 注释作为 `expect`。按它的结论行动："
        "`crop_issue`——用它返回的 `shot` 重新裁那个模板，形状符合控件、紧贴控件、"
        "背后什么都不带，然后 `update_script` 再跑一遍；`wrong_screen`——不要重裁，"
        "去修它前面那一步；`inconclusive`——你自己按下面 1 到 3 步查。\n"
        "\n"
        "1. `view_template(bundle, name)`——它当时在找什么（名字在报错的路径里；"
        "`read_script` 会列出来）。\n"
        "2. `view_crop(region={\"x\":...,\"y\":...,\"w\":...,\"h\":...})`，"
        "区域用报错里的 `box`，或者用 `region={\"cx\":...,\"cy\":...,\"r\":...}` "
        "配它的 `circle`——那里现在是什么，按模板自己的尺度。需要整屏时改用 "
        "`screenshot`。\n"
        "3. 把两者都和失败那一步的 `//` 注释对照：是同一个控件长得不一样了"
        "（一个你从没等到的状态、每次运行都会变的文案），还是根本没有东西"
        "在应它（前一步没落地，或者这根本不是你以为的那个界面）。\n"
        "4. 把原因修一次，再跑一遍：裁切不对就 `update_script` 换掉那个模板，"
        "只是界面还没准备好就加一个 `wait_until` 或把 `ms` 加长。"
        "原样再跑一遍同一个脚本不算修复。\n"
        "5. 这样还失败：说明你现在认为这个界面在干什么，不要再改另一个模板。"
    ),
    "图的流程": (
        "图是一个游戏的各个界面，用 bundle 存：anchor `<graph>.<id>` 是一个界面"
        "——它的 golden.md 说明这是哪个界面，它的 main.ccat 回答这个界面现在是否"
        "在显示——边 `<graph>.<a>-<b>` 是从界面 a 走到界面 b 的那个脚本。"
        "`goto(\"<graph>:<id>\")` 会沿边走进去，每一跳都检查两端。"
        "当 brief 是要在一个游戏里走动，或者某个目标需要图里还没有的界面时，"
        "这就是该干的活——顺序如下：\n"
        "\n"
        "1. `list_scripts`，然后 `read_graph(<graph>)`。图的 bundle 会带着标签："
        "`[graph <graph>, N anchors, M edges, K actions]`、"
        "`[anchor of <graph>, <kind>]`、`[edge of <graph>]` 和 "
        "`[action of <graph> on <anchor>]`。新图从 anchor `entry` 开始："
        "每一次运行都能从那里起步的界面，通常就是桌面。\n"
        "2. 每个界面：`screenshot`，然后 `save_anchor(graph, anchor, golden, "
        "source, templates)`，一次写好 golden.md、main.ccat 和模板。"
        "它会先在最新截图上跑一遍已有的 anchor：被拒绝并回 `already anchor <id>` "
        "说明这个界面已经在图里了——用那个 anchor，不要再存一个。\n"
        "3. 每条边：`save_script(\"<graph>.<a>-<b>\", <那些移动>)`，再用 "
        "`save_template` 存它的模板。回复末尾会附上图检查的结果；"
        "它列出的每个问题都要修。\n"
        "4. 用 `run_script` 验证从界面 a 出发的一条边，源码传 "
        "`run(\"<graph>.<a>-<b>/main.ccat\")`：它会检查起点是 a、终点是 b。"
        "验证整条路径用 `run_script` 加 `goto(\"<graph>:<anchor>\")`；"
        "报错会列出它试过的每一跳和最后看到的界面。\n"
        "5. 起点和终点都在同一个界面上的活——中途可以短暂离开那个界面——是一个 "
        "action `{graph}.{anchor}.{action}`，用 "
        "`save_script(\"{graph}.{anchor}.{action}\")` 和 `save_template` 保存。"
        "`stzb.kucang.claim` 带着 `[action of stzb on kucang]` 标签。"
        "用 `run_script` 加 `run(\"{graph}.{anchor}.{action}/main.ccat\")` 验证。"
    ),
    "图的规则": (
        "  * 一次运行起步的那个界面，anchor id 是 `entry`，即使 brief 里把它"
        "叫作主页或桌面。那个名字写进 golden.md，不要为它开第二个 anchor。"
        "还没有边时，用 `goto(\"<graph>:entry\")` 验证，并说明那个名字不是 "
        "anchor id。返回 `already anchor`，或者 anchor 不可达，说明要用已经"
        "存在的那个 anchor，或者说清冲突在哪里；不要删掉 `entry` 重来。\n"
        "  * anchor 的脚本只做判断：`has`、`if` / `elif` / `else`、`log`、"
        "`return true` / `return false`。每一次 locate 都会跑所有 anchor，"
        "所以它永远不动作，`save_anchor` 也会拒绝会动作的脚本。\n"
        "  * anchor 的模板是那个界面上固定存在、别的界面没有的控件：一个标题、"
        "一个标签页、一个按钮。bundle 里每一张 PNG 都会画在编辑器里的这个 "
        "anchor 上，所以不要为了排除它而存别的界面的图。只为了 `return false` "
        "而写的 `has`——这份界面框架的另一种模式——不切分 anchor。那个模式仍然"
        "属于这个 anchor，它的活是一个 action "
        "`{graph}.{anchor}.{action}`，这次交接即使不能改这个 anchor，也可以"
        "创建它。两个 anchor 在同一个界面上都回答 true，就是一张走不动的图："
        "用只有这个界面才有的图把它们分开，绝不要用阈值。\n"
        "  * golden.md 先是 front matter，然后是对这个界面的一段描述：哪些部分"
        "固定、哪些每次运行都会变。`kind: page`（默认）是你能停在上面发呆的"
        "界面；`kind: transient` 是加载或过渡界面——你可能正处在上面，"
        "但它不是你该去的地方，所以没有边；`kind: popup` 配 "
        "`close: <关掉它的那个 bundle>`，是可以盖在任何界面上的弹窗。\n"
        "  * 一条边可以分支——冷启动和热启动走不同的路——但它不检查自己的两端，"
        "也绝不调用 `goto`：图会检查它从 a 起步，并给 b 20 秒出现的时间。\n"
        "  * 耗时更长的边自己等：对 b 的模板 `wait_until`，"
        "加载很久时用 `loop` + `wait_until`。\n"
        "  * 一条边必须能从任意一端被跑两次：重新规划把你丢到别处之后，"
        "它会被再跑一遍。一条边只会在另一个页面上结束。任何花资源或领奖励的"
        "东西都属于 action，不属于边。\n"
        "  * action 的脚本不调用 `goto`，也不自己 `at`；它中途可以离开页面。"
        "一个任务就是 `goto` 到那个页面，"
        "`run(\"{graph}.{anchor}.{action}/main.ccat\")`，然后核对结果。\n"
    ),
}


def _read_gold_script() -> str:
    """The worked example, read from the app's own checkout rather than copied here.

    One example in one file, edited in one place -- `ccat_help` reads `CCAT.md`
    from beside `config.json` the same way; neither belongs to a skill pack,
    which is bundles only. A missing file is not fatal, since the
    rules above stand on their own, so an empty string is the answer and the
    block is left out.
    """
    path = Path(__file__).resolve().parents[4] / "gold.ccat"
    try:
        return path.read_text(encoding="utf-8").strip()
    except OSError:
        return ""


_example = _read_gold_script()
if _example:
    RULES["示例"] = (
        "下面这些完整的例子遵守了上面的每一条规则——一个 anchor、一条边、"
        "那个 anchor 上的一个 action，以及一个走到那里并跑它的任务。"
        "照着它们的写法写：\n\n" +
        _example
    )

_CCAT_CHEATSHEET = (
    ".ccat 语言。完整参考调 `ccat_help`；下面这些是最容易写错的地方：\n"
    "  * 注释是 `//`；分号可以省略。没有表达式，也没有运行时变量："
    "`defs` 的值在解析时就被替换掉。\n"
    "  * `tap_at(x, y)` / `swipe_at(...)` 的坐标是按截图尺寸归一到 [0,1] 的，"
    "不是像素；超出范围是错误。\n"
    "  * 动作：tap(\"a.png\")、tap_at(x, y)、tap_offset(\"a.png\", dx, dy)、"
    "swipe(\"a.png\", \"b.png\")、wait(ms)、wait_until(\"a.png\", ms)"
    "（超时是错误）、log(\"msg\")、home、run(\"other.ccat\")。\n"
    "  * 控制流：if (has(\"a.png\")) { } else { }，连写用 elif，loop(n) { }，"
    "do { } while (has(\"a.png\"))（50000 次硬性中止），break，return。"
    "`if` / `elif` / `while` 也可以接 `has(\"a.png\")`，它回答那个模板现在"
    "是否在屏幕上；或者接一条会报结果的语句（tap、tap_offset、swipe、tap_at、"
    "swipe_at、wait_until、run），那就先执行它，再按结果选分支：失败算 false，"
    "而光秃秃的 tap 仍然会让脚本失败。`return` / `return true` 回答 true，"
    "`return false` 回答 false，没有 return 的脚本回答 true，"
    "`run(...)` 会把这个回答传回来。\n"
    "  * 某条语句失败会中止脚本，并作为错误回到你这里。需要 PNG 的语句"
    "（`tap`、`tap_offset`、`swipe`、`wait_until`）或需要 `.ccat` 的语句"
    "（`run`）在文件缺失时失败；模板缺失的 `has(...)` 只是 false。\n"
    "  * 图是一个游戏的各个界面：anchor bundle 是 `<graph>.<id>`"
    "（golden.md，加上一个只做判断的 main.ccat），边 bundle 是 "
    "`<graph>.<a>-<b>`。`goto(\"<graph>:<anchor>\")` 走到某个 anchor，"
    "`at(\"<graph>:<anchor>\")` 测你是不是在某个 anchor 上，用于分支。"
    "对一条边 `run` 时也会检查它的两端。\n"
)


SCRIPTER_PERSONA = (
    "你替 operator 写和修 .ccat bundle。你从不和用户说话；你的回复就是一次"
    "工具结果。第一行必须恰好是 `saved`、`updated` 或 `failed` 之一"
    "（这三个词照抄英文，不要翻译），然后用一到三句话说明你做了什么、"
    "这次运行最后怎么样了。回答之前先用工具把活干完：读和写 bundle、"
    "把每个模板都裁好、跑一遍、再按报错修好。brief 就是你能看到的全部对话。"
    "brief 里没有的约束，对你来说不存在。"
)

SCRIPTER_RULE_NAMES = (
    "图的流程", "图的规则",
    "脚本流程", "脚本规则", "失败处理", "示例",
)


def _rules(names: tuple[str, ...]) -> str:
    """One `## name` block per rule, skipping a name the gold example did not add."""
    return "\n\n".join(
        f"## {name}\n\n{RULES[name]}" for name in names if name in RULES)


def scripter_prompt() -> str:
    """Who the scripter is, plus the language reference the operator no longer carries."""
    return SCRIPTER_PERSONA + "\n\n" + _rules(SCRIPTER_RULE_NAMES) + "\n\n" + _CCAT_CHEATSHEET


def _no_summary(messages: list) -> str:
    """What the operator hears when the scripter stops without a sentence."""
    last = ""
    for message in messages:
        if isinstance(message, ToolMessage):
            last = session.content_text(message.content)
    if not last:
        return "failed: 写脚本的 agent 没有给出总结"
    return "failed: 写脚本的 agent 没有给出总结\n" + last


def run_scripter(args: dict, host_tools: list,
                 max_model_rounds: int | None = None) -> str:
    """Run the scripter to the end and return the text the operator should see.

    A failure here is a tool result, not a failed operator turn.
    """
    goal = str(args.get("goal") or "").strip()
    if not goal:
        return "error: goal 为空"
    brief = brief_of(args)
    with observability.span("author-script", brief) as (callbacks, obs):
        def note(text: str) -> str:
            if obs is not None:
                obs.update(output={"reply": text})
            return text

        token = None
        try:
            import graph
            if handoff_may_change(args):
                token = arm_freeze(None)
            else:
                listed = tools.call_cpp("list_scripts", {})
                if not listed.get("ok"):
                    return note("error: " + str(listed.get("error", "")))
                token = arm_freeze(frozenset(
                    bundle_names_of(str(listed.get("text") or ""))))
            if max_model_rounds is None:
                max_model_rounds = graph.MAX_MODEL_ROUNDS
            state = {
                "text": brief,
                "tools": scripter_tools(host_tools),
                "tool_rounds": 0,
                "max_model_rounds": max_model_rounds,
            }
            # This span's callbacks, not the parent's whole config: joining the
            # two Pregel runs makes a picture coming back stall the parent's
            # stream.
            out = graph.run_subgraph(
                _scripter_graph(), state, callbacks or None)
        except tools.TurnInterrupted:
            note("interrupted")
            raise
        except Exception as exc:  # noqa: BLE001 - the operator hears it as a tool error
            return note("error: " + str(exc))
        finally:
            if token is not None:
                disarm_freeze(token)
        if out.get("interrupted"):
            note("interrupted")
            raise tools.TurnInterrupted()
        if out.get("error"):
            return note("error: " + str(out["error"]))
        reply = (out.get("reply") or "").strip()
        if not reply:
            own = (out.get("messages") or [])[out.get("input_len") or 0:]
            return note(_no_summary(own))
        return note(reply)


def _scripter_build(state: dict) -> dict:
    """The scripter's request: its own prompt and history, plus the brief."""
    memorier.compact_scripter()
    messages: list[object] = [SystemMessage(content=(
        scripter_prompt() + memorier.scripter_memory_block(session.scripter_memory())
    ))]
    messages.extend(session.scripter_history())
    frame = session.latest_frame()
    if frame is not None:
        messages.append(frame)
    messages.append(HumanMessage(content=state.get("text", "")))
    # One past the brief, matching `build_messages`: `append_scripter_turn`
    # writes the brief itself, so the slice must not include it again.
    return {"messages": messages, "input_len": len(messages)}


def _scripter_invoke(state: dict) -> dict:
    """One scripter round. Only this round's text is kept.

    The operator accumulates text because the UI streams every round. The
    handoff result is the last round alone: an announcement before a tool
    call must not prefix `saved`, and must not hide an empty ending.
    """
    import graph
    fresh = dict(state)
    fresh["reply"] = ""
    return graph._invoke(fresh, state.get("tools") or [], subagent.chunk_sink)


def _apply_scripter_tools(state: dict) -> dict:
    import graph
    messages = list(state.get("messages") or [])
    calls = getattr(messages[-1], "tool_calls", None) or []

    def run_one(call: dict) -> tuple[object, list]:
        name = call.get("name", "")
        args = call.get("args") or {}
        if name == diagnoser.DIAGNOSE_TEMPLATE_NAME:
            # The shot this diagnosis takes is the one a re-cut is checked
            # against, so it has to stay in the scripter's messages.
            before = session._LATEST_NUM
            with subagent.working(
                    "diagnoser",
                    f"{args.get('bundle', '')}/{args.get('template', '')}") as run:
                content = diagnoser.run_diagnoser(args)
                run.settle(content)
            pics: list = []
            if session._LATEST_NUM != before:
                frame = session.latest_frame()
                if frame is not None:
                    pics.append(frame)
            return content, pics
        frozen = _frozen.get()
        if frozen is not None:
            refusal = refuse_existing(name, args, frozen)
            if refusal:
                return refusal, []
        return graph._host_result(name, args)

    results, pictures, interrupted = graph._run_calls(calls, run_one)
    out = {
        "messages": session.demote_pictures(messages + results + pictures),
        "tool_rounds": state.get("tool_rounds", 0) + 1,
    }
    if interrupted:
        out["interrupted"] = True
    return out


def _route_after_scripter_tools(state: dict) -> str:
    if state.get("interrupted"):
        return "scripter_commit"
    return "scripter_invoke"


def _scripter_commit(state: dict) -> dict:
    """Keep the handoff only when the scripter's model call succeeded."""
    if state.get("error"):
        return {}
    session.append_scripter_turn(
        state.get("text", ""),
        list(state.get("messages") or [])[state.get("input_len") or 0:])
    return {}


def build_scripter_graph():
    import graph
    compiled = StateGraph(graph.TurnState)
    compiled.add_node("scripter_build", _scripter_build)
    compiled.add_node("scripter_invoke", _scripter_invoke)
    compiled.add_node("scripter_tools", _apply_scripter_tools)
    compiled.add_node("scripter_commit", _scripter_commit)
    compiled.set_entry_point("scripter_build")
    compiled.add_edge("scripter_build", "scripter_invoke")
    compiled.add_conditional_edges(
        "scripter_invoke",
        graph.route_after_invoke,
        {"run_tools": "scripter_tools", "commit": "scripter_commit"},
    )
    compiled.add_conditional_edges(
        "scripter_tools",
        _route_after_scripter_tools,
        {"scripter_invoke": "scripter_invoke", "scripter_commit": "scripter_commit"},
    )
    compiled.add_edge("scripter_commit", END)
    return compiled.compile()


_SCRIPTER = None


def _scripter_graph():
    global _SCRIPTER
    if _SCRIPTER is None:
        _SCRIPTER = build_scripter_graph()
    return _SCRIPTER
