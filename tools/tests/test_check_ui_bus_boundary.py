"""Tests for tools/check_ui_bus_boundary.py — the D7 editor.ui boundary gate (M9 e08c, R-QA-013).

The gate's whole value is that it goes RED when a forwarding path is planted, so these tests plant
one of each shape it must catch — a bridge import in the bus module, and a ui-topic subscription
whose callback reaches the exit — alongside the shapes it must NOT flag (prose about the bridge, a
non-ui subscription, a mirror sink, test sources).
"""

from __future__ import annotations

import importlib.util
import sys
from pathlib import Path

import pytest

TOOLS = Path(__file__).resolve().parents[1]


def load_tool(name: str):
    spec = importlib.util.spec_from_file_location(name, TOOLS / f"{name}.py")
    assert spec is not None and spec.loader is not None
    module = importlib.util.module_from_spec(spec)
    sys.modules[name] = module
    spec.loader.exec_module(module)
    return module


check_ui_bus_boundary = load_tool("check_ui_bus_boundary")


CLEAN_BUS = """// The editor.ui bus. It never imports bridge.ts — see the D7 note.
export const UI_TOPIC_FOCUS = "editor.ui.focus";
export class EditorUiBus {
    publish(topic, payload) {
        return { topic, payload };
    }
}
"""

CLEAN_CONSUMER = """import { EditorUiBus, UI_TOPIC_FOCUS } from "./uibus.js";
import { ShellBridge } from "./bridge.js";

export function wire(bus: EditorUiBus, bridge: ShellBridge): void {
    bus.subscribe(UI_TOPIC_FOCUS, (event) => {
        record(event);
    });
    // A NON-ui subscription may talk to the Shell freely — that is the ordinary editor-core path.
    other.subscribe("shell.something", () => void bridge.call("panel.list"));
}
"""


def _tree(root: Path, bus: str = CLEAN_BUS, consumer: str = CLEAN_CONSUMER) -> Path:
    source = root / "src"
    source.mkdir(parents=True, exist_ok=True)
    (source / "uibus.ts").write_text(bus, encoding="utf-8")
    (source / "boot.ts").write_text(consumer, encoding="utf-8")
    return source


def test_clean_tree_passes(tmp_path: Path) -> None:
    source = _tree(tmp_path)
    assert check_ui_bus_boundary.check(source) == []
    assert check_ui_bus_boundary.main(["--source-root", str(source)]) == 0


def test_missing_bus_module_is_a_configuration_error(tmp_path: Path) -> None:
    (tmp_path / "src").mkdir()
    with pytest.raises(FileNotFoundError):
        check_ui_bus_boundary.check(tmp_path / "src")
    assert check_ui_bus_boundary.main(["--source-root", str(tmp_path / "src")]) == 2


def test_missing_source_root_is_a_configuration_error(tmp_path: Path) -> None:
    assert check_ui_bus_boundary.main(["--source-root", str(tmp_path / "nope")]) == 2


@pytest.mark.parametrize(
    "planted",
    [
        'import { ShellBridge } from "./bridge.js";\n' + CLEAN_BUS,
        CLEAN_BUS + "\nexport function forward(b) { return b.call('rpc'); }\n",
        CLEAN_BUS + "\nconst q = globalThis[BRIDGE_QUERY_FUNCTION];\n",
    ],
)
def test_rule_1_a_bus_module_that_names_the_exit_fails(tmp_path: Path, planted: str) -> None:
    """RULE 1: the bus must be structurally incapable of reaching the daemon."""
    source = _tree(tmp_path, bus=planted)
    findings = check_ui_bus_boundary.check(source)
    assert findings, "a planted exit reference in the bus module must be a finding"
    assert "uibus.ts" in findings[0]
    assert check_ui_bus_boundary.main(["--source-root", str(source)]) == 1


def test_rule_1_ignores_prose_about_the_bridge(tmp_path: Path) -> None:
    """The bus module DOCUMENTS the boundary at length; comments are not code."""
    bus = (
        "// This module never imports ShellBridge from bridge.ts, and detectBridgeQuery is\n"
        "/* likewise absent: BRIDGE_QUERY_FUNCTION is the exit this file must never name. */\n"
        + CLEAN_BUS
    )
    assert check_ui_bus_boundary.check(_tree(tmp_path, bus=bus)) == []


@pytest.mark.parametrize(
    "callback",
    [
        "(event) => void bridge.call('editor.select', event)",
        "(event) => { void shellBridge.call('x', event); }",
        "(event) => new ShellBridge(q).call('x', event)",
    ],
)
def test_rule_2_a_ui_subscription_that_reaches_the_exit_fails(tmp_path: Path, callback: str) -> None:
    """RULE 2: the forwarding path a clean bus module cannot prevent."""
    consumer = (
        'import { UI_TOPIC_FOCUS } from "./uibus.js";\n'
        f"bus.subscribe(UI_TOPIC_FOCUS, {callback});\n"
    )
    source = _tree(tmp_path, consumer=consumer)
    findings = check_ui_bus_boundary.check(source)
    assert findings, f"a forwarding subscription must be a finding: {callback}"
    assert "boot.ts" in findings[0]
    assert check_ui_bus_boundary.main(["--source-root", str(source)]) == 1


def test_rule_2_catches_a_GENERIC_subscribe_call(tmp_path: Path) -> None:
    """REGRESSION: the shape editor-core actually writes, and the one the first gate missed.

    `bus.subscribe<ThemeChangedPayload>(UI_TOPIC_THEME_CHANGED, …)` is theme.ts's real call. A bare
    `subscribe\\s*\\(` pattern skips it, so the gate reported a CLEAN sweep on a tree with a planted
    forwarding path in it — found by planting, not by reading. This case pins the fix.
    """
    consumer = (
        'import { UI_TOPIC_THEME_CHANGED } from "./uibus.js";\n'
        "bus.subscribe<ThemeChangedPayload>(UI_TOPIC_THEME_CHANGED, (event) => {\n"
        "    void ShellBridge.detect()?.call('editor.ui.forward', event);\n"
        "});\n"
    )
    findings = check_ui_bus_boundary.check(_tree(tmp_path, consumer=consumer))
    assert len(findings) == 1, "a generic subscribe call must be scanned like any other"


def test_rule_2_catches_a_string_literal_topic_and_a_multiline_callback(tmp_path: Path) -> None:
    consumer = (
        'bus.subscribe("editor.ui.theme-changed", (event) => {\n'
        "    const payload = event.payload;\n"
        "    void bridge.call('themes.push', payload);\n"
        "});\n"
    )
    findings = check_ui_bus_boundary.check(_tree(tmp_path, consumer=consumer))
    assert len(findings) == 1
    assert ":1:" in findings[0], "the finding names the subscribe site, not the inner line"


def test_rule_2_leaves_the_mirror_seam_alone(tmp_path: Path) -> None:
    """The cross-window mirror IS the design; a bridge-backed sink is e10's, and legitimate."""
    consumer = (
        'import { UI_TOPIC_FOCUS } from "./uibus.js";\n'
        "bus.attachMirror({ deliver: (event) => void bridge.call('shell.ui.mirror', event) });\n"
        "bus.subscribe(UI_TOPIC_FOCUS, (event) => paint(event));\n"
    )
    assert check_ui_bus_boundary.check(_tree(tmp_path, consumer=consumer)) == []


def test_test_sources_are_exempt(tmp_path: Path) -> None:
    """uibus.test.ts installs a recording exit on purpose — that IS the runtime half of the proof."""
    source = _tree(tmp_path)
    (source / "test").mkdir()
    (source / "test" / "uibus.test.ts").write_text(
        'bus.subscribe(UI_TOPIC_FOCUS, (e) => void bridge.call("x", e));\n', encoding="utf-8"
    )
    assert check_ui_bus_boundary.check(source) == []
