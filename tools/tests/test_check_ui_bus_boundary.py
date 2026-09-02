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


def test_rule_2_catches_a_NESTED_generic_subscribe_call(tmp_path: Path) -> None:
    """REGRESSION: the SECOND generic bypass, found by planting the shape one refactor away.

    The first fix admitted exactly one level of type argument (`<[^<>()]*>`), so
    `subscribe<Readonly<Record<string, string>>>(…)` was skipped and the gate reported a CLEAN sweep
    with a live forwarding path planted in theme.ts. That payload type is not invented — it is the
    declared type of `ThemeChangedPayload.variables`. Found by planting, not by reading the regex.
    """
    consumer = (
        'import { UI_TOPIC_THEME_CHANGED } from "./uibus.js";\n'
        "bus.subscribe<Readonly<Record<string, string>>>(UI_TOPIC_THEME_CHANGED, (event) => {\n"
        "    void bridge.call('editor.ui.forward', event);\n"
        "});\n"
    )
    findings = check_ui_bus_boundary.check(_tree(tmp_path, consumer=consumer))
    assert len(findings) == 1, "a NESTED generic type argument must not hide the call site"


def test_rule_2_nested_generic_on_a_clean_callback_is_still_clean(tmp_path: Path) -> None:
    """The widened type-argument pattern must not manufacture findings on innocent code."""
    consumer = (
        'import { UI_TOPIC_THEME_CHANGED } from "./uibus.js";\n'
        "bus.subscribe<Readonly<Record<string, string>>>(UI_TOPIC_THEME_CHANGED, (event) => {\n"
        "    paint(event);\n"
        "});\n"
        "// A non-ui subscription may still reach the Shell freely.\n"
        "other.subscribe<Map<string, number>>('shell.something', () => void bridge.call('x'));\n"
    )
    assert check_ui_bus_boundary.check(_tree(tmp_path, consumer=consumer)) == []


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


# ---------------------------------------------------------------------------------------------
# RULE 3 — THE DENY-LIST (editor-UX f1). A mirror-bearing module names no daemon-reaching method.
#
# THE VACUITY THIS FAMILY IS BUILT AGAINST. "A chrome fact cannot cross to the daemon" is satisfiable
# by a check that never runs: a `_MIRROR_BEARING` pattern matching nothing, or a `_DENIED` pattern
# matching nothing, reports a clean sweep over a tree with a live forwarding path in it — exactly what
# this file's own `subscribe` regex did TWICE. So every "it is denied" case below has a sibling in the
# SAME fixture family proving the surface CAN carry a method when the method is allowed (`ui.mirror`,
# the real sink's own target), and a sibling proving a COMPLIANT caller of the very same denied method
# passes. An absence claim has to be earned by a mechanism that demonstrably ran.

MIRROR_SINK = """export class ShellUiMirrorSink implements UiMirrorSink {
    deliver(event) {
        void this.bridge.call(%s, event);
    }
}
"""

# The real sink's own call: a SHELL-local method, which is the design.
ALLOWED_METHOD = '"ui.mirror"'


def test_rule_3_a_shell_local_mirror_sink_passes(tmp_path: Path) -> None:
    """THE POSITIVE HALF. The sink names a method and calls it, and the gate is clean — because the
    method is Shell-local. Without this case, every RED below would be equally consistent with a rule
    that flags any `.call` inside a sink, or with one that never looks at a sink at all."""
    source = _tree(tmp_path, consumer=MIRROR_SINK % ALLOWED_METHOD)
    assert check_ui_bus_boundary.check(source) == []
    assert check_ui_bus_boundary.main(["--source-root", str(source)]) == 0


@pytest.mark.parametrize(
    "spelling",
    [
        '"panel.daemon.call"',
        '"panel.facts.publish"',
        "PANEL_DAEMON_CALL_METHOD",
        "PANEL_FACTS_PUBLISH_METHOD",
    ],
)
def test_rule_3_a_mirror_sink_that_targets_a_daemon_method_fails(
    tmp_path: Path, spelling: str
) -> None:
    """Both daemon-reaching methods, in both spellings — the wire literal AND the editor-core constant
    that mirrors it. Either one is enough to make the call, so either one is a finding."""
    source = _tree(tmp_path, consumer=MIRROR_SINK % spelling)
    findings = check_ui_bus_boundary.check(source)
    assert len(findings) == 1, f"a mirror sink pointed at {spelling} must be a finding"
    assert "MIRROR seam" in findings[0], "reported as the mirror finding, not as rule 1 or rule 2"
    assert "boot.ts" in findings[0]
    assert check_ui_bus_boundary.main(["--source-root", str(source)]) == 1


def test_rule_3_an_inline_attachMirror_sink_is_a_mirror_surface_too(tmp_path: Path) -> None:
    """The sink need not be a class: `attachMirror({ deliver })` is the same seam, written inline."""
    consumer = (
        'bus.attachMirror({ deliver: (event) => void bridge.call("panel.facts.publish", event) });\n'
    )
    findings = check_ui_bus_boundary.check(_tree(tmp_path, consumer=consumer))
    assert len(findings) == 1
    assert "panel.facts.publish" in findings[0]


def test_rule_3_catches_helper_and_local_constant_indirection(tmp_path: Path) -> None:
    """THE BYPASS CLASS THE WHOLE-MODULE SCOPE EXISTS FOR, and why rule 3 is not region-scoped.

    A rule that read only the sink's class body — or only the `attachMirror` argument — is walked
    straight past by a file-local helper called from `deliver`, spelled through a file-local constant
    so neither the literal nor the mirrored constant name appears at the call site. Holding the whole
    module clean needs no regex for either shape. Planted in the REAL `uimirror.ts` as well.
    """
    consumer = (
        'const ONWARD = "panel.daemon.call";\n'
        "function forwardOnward(bridge, event) { void bridge.call(ONWARD, event); }\n"
        "export class ShellUiMirrorSink implements UiMirrorSink {\n"
        "    deliver(event) { forwardOnward(this.bridge, event); }\n"
        "}\n"
    )
    findings = check_ui_bus_boundary.check(_tree(tmp_path, consumer=consumer))
    assert len(findings) == 1, "the indirected forwarding path must still be a finding"
    assert "panel.daemon.call" in findings[0]


def test_rule_3_leaves_a_COMPLIANT_daemon_caller_alone(tmp_path: Path) -> None:
    """THE DISCRIMINATOR. A panel's own daemon call, made from a panel surface, is the design (M9
    e13c-1 and editor-UX d2). The deny-list narrows the MIRROR seam; it is not a blanket ban."""
    consumer = (
        'export const PANEL_DAEMON_CALL_METHOD = "panel.daemon.call";\n'
        'export const PANEL_FACTS_PUBLISH_METHOD = "panel.facts.publish";\n'
        "export function makePackageDaemonCall(bridge, packageId) {\n"
        "    return (method, params) =>\n"
        "        bridge.call(PANEL_DAEMON_CALL_METHOD, { packageId, method, params });\n"
        "}\n"
        "export function makePackageFactPublish(bridge, packageId) {\n"
        "    return (topic, payload) =>\n"
        "        bridge.call(PANEL_FACTS_PUBLISH_METHOD, { packageId, topic, payload });\n"
        "}\n"
    )
    assert check_ui_bus_boundary.check(_tree(tmp_path, consumer=consumer)) == []


def test_rule_3_a_module_that_only_BRINGS_UP_the_mirror_is_not_a_sink(tmp_path: Path) -> None:
    """boot.ts's REAL shape, and the sharpest over-reading control in this family.

    It defines `PANEL_DAEMON_CALL_METHOD` and it starts the cross-window mirror — but by handing a
    bridge to `wireUiMirror`, not by holding a sink. Bringing a transport up is not mirroring, so it
    stays green; the moment it attaches a sink of its own, the parametrized case above applies to it.
    """
    consumer = (
        'export const PANEL_DAEMON_CALL_METHOD = "panel.daemon.call";\n'
        "export function boot(bridge, bus) {\n"
        "    const uiMirror = wireUiMirror(bridge, bus);\n"
        "    const daemonCall = (m, p) => bridge.call(PANEL_DAEMON_CALL_METHOD, { m, p });\n"
        "    return { uiMirror, daemonCall };\n"
        "}\n"
    )
    assert check_ui_bus_boundary.check(_tree(tmp_path, consumer=consumer)) == []


def test_rule_3_ignores_prose_about_the_daemon_methods(tmp_path: Path) -> None:
    """uimirror.ts DOCUMENTS at length why it targets a Shell-local method; comments are not code."""
    consumer = (
        "// It targets a SHELL-local method, never panel.daemon.call — see the D7 note.\n"
        "/* panel.facts.publish would put this window's chrome facts on the daemon wire. */\n"
        + MIRROR_SINK % ALLOWED_METHOD
    )
    assert check_ui_bus_boundary.check(_tree(tmp_path, consumer=consumer)) == []


def test_rule_1_the_bus_module_may_not_name_a_daemon_method(tmp_path: Path) -> None:
    """The bus holds no client, so a daemon method name there could only be there to be handed to
    something that does — and it names no exit, so rule 1's exit scan alone would not see it."""
    bus = CLEAN_BUS + '\nexport const ONWARD = "panel.facts.publish";\n'
    findings = check_ui_bus_boundary.check(_tree(tmp_path, bus=bus))
    assert len(findings) == 1, "a daemon method named in the bus module must be a finding"
    assert "uibus.ts" in findings[0] and "panel.facts.publish" in findings[0]


def test_rule_2_a_ui_subscription_that_names_a_daemon_method_fails(tmp_path: Path) -> None:
    """The forwarding callback that names NO exit: the bridge lives inside the helper it calls, so
    only the method name gives it away."""
    consumer = (
        'import { UI_TOPIC_FOCUS } from "./uibus.js";\n'
        'bus.subscribe(UI_TOPIC_FOCUS, (event) => void send("panel.daemon.call", event));\n'
    )
    findings = check_ui_bus_boundary.check(_tree(tmp_path, consumer=consumer))
    assert len(findings) == 1
    assert "subscription" in findings[0]


def test_rule_2_a_NON_ui_subscription_may_still_name_a_daemon_method(tmp_path: Path) -> None:
    """The sibling proving the topic filter still discriminates: this subscription is not editor.ui,
    and an ordinary editor-core module talking to the daemon is the ordinary case."""
    consumer = 'other.subscribe("shell.something", (e) => void send("panel.daemon.call", e));\n'
    assert check_ui_bus_boundary.check(_tree(tmp_path, consumer=consumer)) == []


def test_the_denylist_entries_still_exist_in_the_cpp_headers() -> None:
    """A DENY-LIST OF NAMES GOES STALE SILENTLY — this is what stops that.

    Rename `kPanelDaemonCallMethod`'s value and the checker would sweep clean forever while the new
    method forwarded chrome facts unguarded. `webui-panel-contract` byte-compares the C++ header
    against the BUILT bundle; nothing compared it against THIS list. Now something does: every
    deny-list entry must still be defined, with that exact literal, in the header it names.

    IT READS THE HEADER THROUGH `check_webui_assets._read_cpp_string_constant`, not through a
    substring scan, and that is the whole difference between this test working and only appearing to.
    That reader STRIPS COMMENTS FIRST, for a reason its own docstring states: these headers document
    the very vocabulary being pinned, so a prose line of the form `kFoo = "panel.facts.publish"` left
    above a drifted declaration satisfies a raw `expected in header_text` check and reports green
    across exactly the rename this test exists to catch. `package_facts.h` already names
    `kPanelDaemonCallMethod` in prose today, so that is a live shape here, not a hypothetical one.
    """
    check_webui_assets = load_tool("check_webui_assets")
    headers = Path(__file__).resolve().parents[2] / "src/editor/shell/include/context/editor/shell"
    assert headers.is_dir(), f"the Shell header directory moved: {headers}"
    for literal, _constant, header_name, cpp_constant in check_ui_bus_boundary._DENIED_METHODS:
        header = headers / header_name
        assert header.is_file(), f"deny-list entry {literal} names a header that does not exist"
        assert check_webui_assets._read_cpp_string_constant(header, cpp_constant) == literal, (
            f"the deny-list is STALE: {header_name} no longer defines {cpp_constant} as "
            f'"{literal}". The Shell method was renamed and tools/check_ui_bus_boundary.py still '
            f"denies the old name."
        )


def test_rule_2_a_daemon_method_hit_does_not_advise_the_mirror_seam(tmp_path: Path) -> None:
    """The remedy in the finding must not point at the very construct rule 3 forbids.

    The exit-by-name finding says "if this is the cross-window MIRROR, use a UiMirrorSink instead" —
    right for a bridge in the callback, WRONG for a daemon method: moving that call into a sink is
    what rule 3 denies, and moving the sink one module out to dodge rule 3 is the residual the
    checker names, not a fix. A developer following the old text would have walked straight into it.
    """
    consumer = (
        'import { UI_TOPIC_FOCUS } from "./uibus.js";\n'
        'bus.subscribe(UI_TOPIC_FOCUS, (event) => void send("panel.facts.publish", event));\n'
    )
    findings = check_ui_bus_boundary.check(_tree(tmp_path, consumer=consumer))
    assert len(findings) == 1
    assert "NOT the mirror seam" in findings[0]
    assert "package_facts.h kPanelFactsPublishMethod" in findings[0], "and it names where to look"


def test_the_cross_module_sink_factory_is_a_KNOWN_residual(tmp_path: Path) -> None:
    """THE HOLE THIS GATE DOES NOT CLOSE, pinned so it cannot be mistaken for coverage.

    Rule 3's scope is a MODULE, and "mirror-bearing" is two names. Build the sink OBJECT in a module
    that names neither — a factory returning `{ deliver }` — and attach it from a module that names
    no denied method, and both halves sweep clean with a live forwarding path in the tree. Whole-
    module scope beats the FILE-local helper it was chosen for; it does not beat a cross-module one.

    This test asserts the CURRENT behaviour so the residual is MEASURED rather than assumed. If a
    later task strengthens rule 3 to catch this shape, this test goes RED — and that red is the
    signal to delete it and strike the residual from the checker docstring and docs/editor-ui-bus.md.
    """
    source = _tree(tmp_path)
    (source / "onwardsink.ts").write_text(
        'import { PANEL_FACTS_PUBLISH_METHOD } from "./packagefacts.js";\n'
        "export function makeOnwardSink(bridge) {\n"
        "    return { deliver: (e) => void bridge.call(PANEL_FACTS_PUBLISH_METHOD, e) };\n"
        "}\n",
        encoding="utf-8",
    )
    (source / "onwardwire.ts").write_text(
        'import { makeOnwardSink } from "./onwardsink.js";\n'
        "export function wireOnward(bridge, bus) {\n"
        "    return bus.attachMirror(makeOnwardSink(bridge));\n"
        "}\n",
        encoding="utf-8",
    )
    assert check_ui_bus_boundary.check(source) == [], (
        "KNOWN RESIDUAL (see check_ui_bus_boundary.py § RESIDUAL): a sink built one module out is "
        "not mirror-bearing, so rule 3 does not see it. If this now FAILS, rule 3 got stronger — "
        "delete this test and strike the residual from the docstring and docs/editor-ui-bus.md."
    )
