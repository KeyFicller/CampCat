"""Agent visibility and bundle write targets for the host's reflected tools."""

# Unknown tools retain the operator fallback; registry coverage requires an
# explicit decision for every tool compiled into the current host.
OPERATOR_ONLY = frozenset({"tap", "start_emulator", "go_home", "run_bundle", "locate"})
SCRIPTER_ONLY = frozenset({
    "read_script", "ccat_help", "save_script", "update_script", "delete_script",
    "rename_script", "save_template", "reuse_template", "view_template", "view_crop", "save_anchor",
})
SHARED = frozenset({"list_scripts", "run_script", "screenshot", "view_shot", "read_graph"})
HIDDEN = frozenset({"mark", "tight_crop"})
SOLO = frozenset({"screenshot", "tap", "start_emulator", "go_home"})

WRITE_FIELDS = {
    "save_script": ("name",),
    "update_script": ("name",),
    "delete_script": ("name",),
    "rename_script": ("name", "new_name"),
    "save_template": ("bundle",),
    "reuse_template": ("bundle",),
    "save_anchor": ("graph", "anchor"),
}


def name_of(tool: dict) -> str | None:
    try:
        name = tool["function"]["name"]
    except (KeyError, TypeError):
        return None
    return name if isinstance(name, str) else None


def write_targets(tool: str, args: dict) -> list[str]:
    values = [args.get(field) for field in WRITE_FIELDS.get(tool, ())]
    if tool == "save_anchor":
        return [".".join(values)] if all(isinstance(value, str) and value for value in values) else []
    return [value for value in values if isinstance(value, str) and value]
