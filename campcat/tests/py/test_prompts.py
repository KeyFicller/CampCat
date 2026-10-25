"""The two prompts. The operator never sees the script language; the scripter does."""

import agent.operator as operator
import agent.region_review as region_review
import agent.scripter as scripter
from agent.region_rules import REGION_RULES


def test_operator_prompt_drops_the_script_language():
    text = operator.operator_prompt()
    assert "author_script" in text
    assert "go_home" in text
    assert "原样" in text
    assert "goto(" in text
    assert "read_graph" in text
    assert "wait_until" not in text
    assert "read_script" not in text


def test_scripter_prompt_keeps_the_cheatsheet():
    text = scripter.scripter_prompt()
    assert "wait_until" in text
    assert "saved" in text
    # The graph half: the workflow that builds one, and the language it uses.
    assert "save_anchor" in text
    assert "golden.md" in text
    assert "不是 anchor id" in text
    assert "不要为了排除它而存别的界面的图" in text
    assert 'has("other.png")' not in text
    assert "goto(" in text
    assert "update_graph" not in text
    assert "save_golden" not in text
    assert "graph.json" not in text
    # Building the graph comes before scripting on it.
    assert text.index("## 图的流程") < text.index("## 脚本流程")


def test_scripter_prompt_crops_an_element_once():
    text = scripter.scripter_prompt()
    assert "只调一次 `view_crop`" in text
    assert "再收紧一点" in text
    assert "线以内就是 `save_template` 保存的内容" in text
    assert "16 像素" in text
    assert "形状由你根据控件外轮廓自己挑" in text
    assert "菱形（菱形" in text


def test_scripter_prompt_uses_an_action_for_same_screen_work():
    text = scripter.scripter_prompt()
    assert "{graph}.{anchor}.{action}" in text
    assert "[action of stzb on kucang]" in text
    assert "同一个界面" in text
    assert "另一种模式" in text
    assert "不切分 anchor" in text
    assert text.index("{graph}.{anchor}.{action}") < text.index("## 脚本流程")


def test_operator_hands_off_at_a_new_screen_and_at_a_finished_task():
    # The cadence is the point: an operator that walks the whole flow first
    # has already left the screen whose anchor it was supposed to add.
    text = operator.operator_prompt()
    assert "不要等整轮结束" in text
    assert "回答 `unknown` 的界面就是一个新 anchor" in text
    assert "连着开两个新 anchor" in text
    # An action is the graph's third part, named as the scanner names it: a
    # handoff that does not say so leaves `actions: 0` and nobody notices.
    assert "<graph>.<anchor>.<action>" in text
    assert "花掉资源或领取资源" in text
    assert "中途可以离开这个页面" in text
    assert "`actions: 0`" in text


def test_prompts_freeze_existing_bundles():
    assert "fix: true" in operator.operator_prompt()
    text = scripter.scripter_prompt()
    assert "fix: true" in text
    assert "last_error" in text
    assert "update_script` before you duplicate" not in text


def test_prompt_override_covers_only_the_operator(monkeypatch):
    untouched = scripter.scripter_prompt()
    monkeypatch.setenv("CAMPCAT_LLM_SYSTEM_PROMPT", "OVERRIDE")
    assert operator.operator_prompt() == "OVERRIDE"
    assert scripter.scripter_prompt() == untouched
    assert "OVERRIDE" not in scripter.scripter_prompt()


def test_solo_prompt_is_its_own_persona():
    # Appending a note to a persona that names eight tools leaves the model
    # reading about tools it does not have; solo gets its own text instead.
    text = operator.operator_prompt(True)
    assert text.startswith(operator.SOLO_PERSONA)
    assert operator.OPERATOR_PERSONA not in text
    for gone in ("author_script", "run_script", "list_scripts", "read_graph"):
        assert gone not in text
    assert "`screenshot`、`tap`、`go_home` 和 `start_emulator`" in text
    assert "单独调用" in text  # the boot blocks, so it is asked for solo
    assert "## 格式" in text  # the shared rules still ride along


def test_ordinary_prompt_still_offers_the_whole_device():
    text = operator.operator_prompt()
    assert text.startswith(operator.OPERATOR_PERSONA)
    assert operator.SOLO_PERSONA not in text


def test_every_prompt_that_proposes_a_region_shares_the_rules():
    # The rules come from the redrawn samples. A copy left behind in one
    # prompt is how the proposer and the check start disagreeing about what a
    # good region is.
    assert REGION_RULES in operator.operator_prompt()
    assert REGION_RULES in operator.operator_prompt(True)
    assert REGION_RULES in scripter.scripter_prompt()
    assert "宽得远超高的菱形" in region_review.REVIEW_CRITERIA["template"]


def test_the_check_is_pushed_to_correct_before_accepting():
    # A rubber stamp is the failure worth tuning against: the check is the only
    # reader that sees the boundary before the matcher has to live with it.
    system = region_review._review_system("template")
    assert "先假定这个区域是错的" in system
    assert "这次检查消失很久之后" in system
    assert "低的只是多买一次复查" in system
    # It also reviews taps, where the same pressure applies.
    assert "先假定这个区域是错的" in region_review._review_system("tap")


def test_a_text_control_is_cropped_to_its_glyphs():
    # The button behind a caption changes with state; the glyphs do not. Both
    # the proposer and the check have to say so, or one pads and one trims.
    assert "先看文字边界" in REGION_RULES
    assert "文字边界。" in region_review.REVIEW_CRITERIA["template"]
    assert "文字边界。" in region_review.TEMPLATE_PICK_PROMPT
