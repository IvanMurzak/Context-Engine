#!/usr/bin/env python3
"""Fail on any path by which an `editor.ui` chrome fact could reach the daemon (M9 e08c, D7).

Design 05 §5: the `editor.ui` bus is EDITOR-LOCAL and its facts are "NOT forwarded to the daemon
(D7)". The task that landed the bus required that boundary to be *proven, not stated* — and a runtime
test alone cannot prove it, because it only observes the code paths it happens to drive. This is the
static half: it sweeps EVERY editor-core source, so a forwarding path nothing drives is still caught.

Three rules, and every one was verified by PLANTING a violation and watching this script go red.

RULE 1 — THE BUS MODULE IS STRUCTURALLY INCAPABLE OF REACHING THE DAEMON.
    editor-core's ONE way out is `ShellBridge` (src/bridge.ts): it wraps CEF's injected query
    function, and the Shell holds the daemon socket and the attach token behind it. `uibus.ts`
    therefore must not import that module, name that class, or otherwise mention the bridge — with no
    reference to the exit, no code in it can forward anything anywhere. It must also not INVOKE a
    `.call(`-shaped method, which closes the one route a name scan cannot see: a bridge handed in
    under an innocuous name. Nor may it name a DENY-LISTED method (rule 3): the bus holds no client,
    so the only thing it could do with such a name is hand it to something that does.

RULE 2 — NO ui-TOPIC SUBSCRIPTION ANYWHERE REACHES THE EXIT.
    Rule 1 keeps the bus clean; it says nothing about a CONSUMER. `bus.subscribe("editor.ui.focus",
    (e) => void bridge.call(...))` in any other editor-core module would forward chrome facts out of
    the renderer while leaving uibus.ts pristine. So every `subscribe(` call whose topic argument
    names a ui topic — a `UI_TOPIC_*` constant or an `editor.ui.*` literal — has its callback text
    scanned for the exit, and a hit is a FINDING. Since editor-UX `f1` the same callback text is also
    scanned for a DENY-LISTED method (rule 3), because a callback that reaches the daemon through a
    HELPER holding the bridge names no exit of its own.

RULE 3 — NO MIRROR-BEARING MODULE NAMES A DAEMON-REACHING METHOD (the deny-list; editor-UX `f1`).
    Rules 1 and 2 both leave the cross-window MIRROR seam (`UiMirrorSink`) alone, and must: mirroring
    chrome facts between the Shell's OWN windows is the design (05 §5), and `uimirror.ts`'s real sink
    legitimately calls a SHELL-local bridge method (`ui.mirror`). A sink is not a subscription, so
    rule 2 never looks at it — which is precisely why the seam needs a rule of its own.

    THE PREMISE THAT USED TO COVER THAT GAP EXPIRED WITH M9 e13c-1. This docstring once argued that a
    sink could not reach the daemon because no method the Shell routed reached it through — every one
    was Shell-local (`panel.*`, `themes.get`, `config.*`, `keybindings.get`, `editor.state.*`,
    `welcome.*`). Two now are not:

      * `panel.daemon.call` (M9 e13c-1, `package_sessions.h` `kPanelDaemonCallMethod`) forwards a
        renderer-supplied method name and params onto a package's baseline daemon session;
      * `panel.facts.publish` (editor-UX `d2`, `package_facts.h` `kPanelFactsPublishMethod`) carries a
        package's declared fact onto that same session.

    Either, named from a mirror sink, puts this window's chrome facts on the daemon wire — the D7
    violation this whole file exists to prevent. So: a module is MIRROR-BEARING when it implements
    `UiMirrorSink` or calls `attachMirror(`, and a mirror-bearing module may not name a deny-listed
    method — by wire literal or by the TS constant that mirrors it — ANYWHERE in the module.

    WHOLE-MODULE, deliberately, and it is the same structural-incapability argument rule 1 makes one
    module out: a region-scoped rule (the sink's class body, the `attachMirror` argument) is bypassed
    by a file-local helper, a file-local `const M = "panel.daemon.call"`, or a renaming import, and
    each patch for those is another regex that has to be right. Holding the whole module clean needs
    none of them — with no daemon method named in the file, no code in it can forward anything to the
    daemon. It costs the seam nothing: `uibus.ts` and `uimirror.ts` are the only mirror-bearing
    modules and neither has any business naming a daemon verb.

    IT DISCRIMINATES RATHER THAN BLANKET-FAILING, which is the property that makes it a deny-list and
    not a ban. A COMPLIANT use of either method — `makePackageDaemonCall` (`boot.ts`) and
    `makePackageFactPublish` (`packagefacts.ts`), each forwarding a PANEL's own call from a panel
    surface — sits in a module that bears no sink and stays green. `boot.ts` is the sharp case: it
    both defines `PANEL_DAEMON_CALL_METHOD` and brings the mirror up (via `wireUiMirror`), and it
    passes, because bringing a transport up is not holding a sink. Attach one there and it fails.

    RESIDUAL, named rather than papered over: the deny-list is a list of NAMES, so a third
    daemon-reaching router method added later is not covered until it is added here, and a sink that
    reached one through a constant imported from another module under a fresh name would not be
    matched by name. `webui-panel-contract` byte-compares both entries against the C++ headers and
    the built bundle, and `test_check_ui_bus_boundary.py` asserts each deny-listed literal still
    exists in the header that defines it — so a RENAME cannot silently empty this list.

It is a SOURCE scan (like tools/check_no_raw_key_handlers.py), so it runs on every default `build` leg
with no browser — registered as the `webui-uibus-boundary` ctest in the `webui-*` family.

Exit codes: 0 = clean, 1 = at least one boundary violation, 2 = a configuration error.
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

# The bus module, relative to the scanned source root. Named rather than discovered: a gate that
# cannot find its subject must fail loudly (exit 2), not pass vacuously.
BUS_MODULE = "uibus.ts"

# THE EXIT. Any mention of editor-core's one way out. `\bbridge\b` covers a `bridge.call(...)` on a
# parameter/field of any of the usual names; `ShellBridge` covers constructing or importing one;
# `BRIDGE_QUERY_FUNCTION`/`detectBridgeQuery` cover reaching for the injected function directly.
_EXIT = re.compile(r"\bbridge\b|\bShellBridge\b|\bdetectBridgeQuery\b|\bBRIDGE_QUERY_FUNCTION\b",
                   re.IGNORECASE)

# RULE 1 is stricter than rule 2, because it can afford to be. A bridge handed to the bus module
# under an innocuous name (`publish(topic, p) { return sink.call("rpc", p); }`) mentions no exit by
# name, so the name scan alone would miss it — and the bus has no legitimate need to INVOKE a
# `.call(`-shaped method at all (it holds no client, and `Function.prototype.call` is not a style
# this file uses). Forbidding the shape outright closes the smuggling route by construction.
_BUS_EXIT = re.compile(_EXIT.pattern + r"|\.call\s*\(", re.IGNORECASE)

# THE DENY-LIST (rule 3). The router methods that reach the DAEMON, each paired with the editor-core
# constant that mirrors it. Both spellings are denied because either is enough to make the call: the
# literal is what goes on the wire, the constant is how editor-core actually writes it.
#
# The two entries are the complete set of daemon-reaching methods the Shell routes today — every
# other routed method is Shell-local. `tools/check_webui_assets.py --panel-contract` byte-compares
# both against the C++ headers and the BUILT bundle, and `test_check_ui_bus_boundary.py` re-reads the
# headers to assert these literals still exist there, so a rename cannot quietly empty this list.
_DENIED_METHODS: tuple[tuple[str, str, str], ...] = (
    ("panel.daemon.call", "PANEL_DAEMON_CALL_METHOD", "package_sessions.h kPanelDaemonCallMethod"),
    ("panel.facts.publish", "PANEL_FACTS_PUBLISH_METHOD", "package_facts.h kPanelFactsPublishMethod"),
)

_DENIED = re.compile(
    "|".join(
        rf"{re.escape(literal)}|\b{re.escape(constant)}\b"
        for literal, constant, _ in _DENIED_METHODS
    )
)

# What makes a module MIRROR-BEARING: it implements the sink interface, or it attaches one. Naming
# the TYPE is enough — a sink is an object with one method, so `implements UiMirrorSink`,
# `: UiMirrorSink` and `satisfies UiMirrorSink` all declare one, and a bare mention in code (as
# opposed to a comment, which `_strip_comments` has already removed) means this module handles sinks.
_MIRROR_BEARING = re.compile(r"\bUiMirrorSink\b|\battachMirror\s*\(")

# The subscribe call site. The OPTIONAL TYPE ARGUMENT is load-bearing: editor-core's own real call is
# `bus.subscribe<ThemeChangedPayload>(UI_TOPIC_THEME_CHANGED, …)`, and a bare `subscribe\s*\(` pattern
# silently skipped every generic one — which is how this gate passed a PLANTED forwarding path on its
# first run. Found by planting a violation, not by reading the regex.
#
# The type argument must admit NESTING. A first fix spelled it `<[^<>()]*>`, which matches exactly one
# level and therefore skipped `subscribe<Readonly<Record<string, string>>>(…)` — not a synthetic shape:
# that is literally the type of `ThemeChangedPayload.variables`, one plausible refactor away from being
# a real call site. Planting that shape passed the gate CLEAN with a live forwarding path in the tree,
# the same bypass class one level deeper. `[^()]*?` admits any depth of `<>`, stays non-greedy so it
# stops at the FIRST `>` that is followed by the call's `(`, and still cannot run across a call
# boundary because parentheses are excluded.
_SUBSCRIBE = re.compile(r"\bsubscribe\s*(?:<[^()]*?>)?\s*\(")

# A ui-topic argument: the exported constants, or a literal in the reserved namespace.
_UI_TOPIC_ARG = re.compile(r"""\bUI_TOPIC_[A-Z_]+\b|['"]editor\.ui\.[a-z0-9.-]*['"]""")

_TS_EXTENSIONS = {".ts", ".tsx", ".mts", ".cts"}


def _is_test_source(path: Path) -> bool:
    """Test sources are exempt: uibus.test.ts installs a recording exit ON PURPOSE (that IS the
    runtime half of this boundary's proof), and asserting on the bridge there is not forwarding."""
    return any(part in {"test", "tests"} for part in path.parts)


def _strip_comments(text: str) -> str:
    """Blank out block/line comments so a prose mention of the bridge is never a finding.

    Replaces with spaces rather than deleting, so reported line numbers stay true.
    """
    out: list[str] = []
    index = 0
    length = len(text)
    while index < length:
        if text.startswith("//", index):
            end = text.find("\n", index)
            end = length if end == -1 else end
            out.append(" " * (end - index))
            index = end
        elif text.startswith("/*", index):
            end = text.find("*/", index + 2)
            end = length if end == -1 else end + 2
            out.append("".join(c if c == "\n" else " " for c in text[index:end]))
            index = end
        else:
            out.append(text[index])
            index += 1
    return "".join(out)


def _balanced_call(text: str, open_index: int) -> tuple[str, int]:
    """Return the argument text of the call whose `(` sits at `open_index`, plus the index after it.

    Naive w.r.t. parentheses inside string literals — deliberately: over-reading a call's extent can
    only make this gate MORE suspicious, never less, and a fail-closed scanner is the right bias for
    a boundary check.
    """
    depth = 0
    for index in range(open_index, len(text)):
        char = text[index]
        if char == "(":
            depth += 1
        elif char == ")":
            depth -= 1
            if depth == 0:
                return text[open_index + 1 : index], index + 1
    return text[open_index + 1 :], len(text)


def _denied_home(token: str) -> str:
    """Where the deny-listed spelling `token` is defined C++-side — quoted in the finding."""
    for literal, constant, home in _DENIED_METHODS:
        if token in (literal, constant):
            return home
    return "the Shell's router"


def check_bus_module(source: str) -> list[str]:
    """RULE 1: the bus module must not name — or invoke — editor-core's exit (comments excluded).

    It must not name a deny-listed daemon method either: the bus holds no client, so a method name
    there could only be there to be handed to something that does.
    """
    findings: list[str] = []
    code = _strip_comments(source)
    for number, line in enumerate(code.splitlines(), start=1):
        if _BUS_EXIT.search(line):
            findings.append(
                f"{BUS_MODULE}:{number}: the editor.ui bus names editor-core's ONE exit "
                f"({line.strip()}) — D7 requires it to be structurally incapable of reaching the "
                f"daemon (design 05 §5)"
            )
            continue
        denied = _DENIED.search(line)
        if denied is not None:
            findings.append(
                f"{BUS_MODULE}:{number}: the editor.ui bus names the DAEMON-reaching method "
                f"`{denied.group(0)}` ({_denied_home(denied.group(0))}) — the bus holds no client, so "
                f"the only use for that name here is handing it to something that does (D7, design "
                f"05 §5)"
            )
    return findings


def check_mirror_module(path_label: str, source: str) -> list[str]:
    """RULE 3: a module that implements or attaches a `UiMirrorSink` names no daemon-reaching method.

    Whole-module, like rule 1 and for the same reason: with no daemon verb named in the file, no code
    in it can route a chrome fact to the daemon — no matter which helper, local constant or renamed
    import the forwarding path is spelled through. A module that bears no sink is untouched, which is
    what keeps a COMPLIANT `panel.daemon.call` / `panel.facts.publish` caller green.
    """
    code = _strip_comments(source)
    if _MIRROR_BEARING.search(code) is None:
        return []
    findings: list[str] = []
    for number, line in enumerate(code.splitlines(), start=1):
        denied = _DENIED.search(line)
        if denied is None:
            continue
        findings.append(
            f"{path_label}:{number}: a module that handles the editor.ui MIRROR seam names the "
            f"DAEMON-reaching method `{denied.group(0)}` ({_denied_home(denied.group(0))}) — a mirror "
            f"sink must target a SHELL-local method, because the Shell mirrors chrome facts between "
            f"its OWN windows and never forwards them to the daemon (D7, design 05 §5). Keep the "
            f"daemon call in a module that bears no sink."
        )
    return findings


def check_subscriptions(path_label: str, source: str) -> list[str]:
    """RULE 2: no `editor.ui.*` subscription callback may reach the exit."""
    findings: list[str] = []
    code = _strip_comments(source)
    for match in _SUBSCRIBE.finditer(code):
        args, _ = _balanced_call(code, match.end() - 1)
        head = args.split(",", 1)
        if not _UI_TOPIC_ARG.search(head[0]):
            continue
        callback = head[1] if len(head) > 1 else ""
        # The exit by name, OR a deny-listed daemon method: a callback that forwards through a HELPER
        # holding the bridge names no exit of its own, and the method name is what gives it away.
        hit = _EXIT.search(callback) or _DENIED.search(callback)
        if hit is None:
            continue
        number = code.count("\n", 0, match.start()) + 1
        findings.append(
            f"{path_label}:{number}: an editor.ui subscription reaches editor-core's exit "
            f"(`{hit.group(0)}`) — a ui-chrome fact must NEVER be forwarded off this renderer (D7, "
            f"design 05 §5). If this is the cross-window MIRROR, use a UiMirrorSink instead."
        )
    return findings


def check(source_root: Path) -> list[str]:
    """Run all three rules over `source_root`. Raises FileNotFoundError when the bus module is missing."""
    bus = source_root / BUS_MODULE
    if not bus.is_file():
        raise FileNotFoundError(f"{bus} does not exist")
    findings = check_bus_module(bus.read_text(encoding="utf-8"))
    for path in sorted(source_root.rglob("*")):
        if not path.is_file() or path.suffix not in _TS_EXTENSIONS or _is_test_source(path):
            continue
        label = path.relative_to(source_root).as_posix()
        source = path.read_text(encoding="utf-8")
        findings.extend(check_subscriptions(label, source))
        # Rule 3 skips the bus module itself: rule 1 already holds it to a stricter standard, and
        # `uibus.ts` is mirror-bearing (it DECLARES the sink interface), so scanning it twice would
        # report the same line under two rules.
        if label != BUS_MODULE:
            findings.extend(check_mirror_module(label, source))
    return findings


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--source-root",
        required=True,
        help="editor-core's TS source root (the directory holding uibus.ts)",
    )
    args = parser.parse_args(argv)

    root = Path(args.source_root)
    if not root.is_dir():
        print(f"[uibus-boundary] ERROR: --source-root {root} is not a directory", file=sys.stderr)
        return 2
    try:
        findings = check(root)
    except FileNotFoundError as exc:
        # The subject moved or was renamed. A gate that cannot find what it guards must FAIL, not
        # report a clean sweep of nothing.
        print(
            f"[uibus-boundary] ERROR: {exc} — the editor.ui bus module moved; point "
            f"tools/check_ui_bus_boundary.py at it (a gate that cannot find its subject is not a gate)",
            file=sys.stderr,
        )
        return 2

    if findings:
        for finding in findings:
            print(f"[uibus-boundary] FINDING: {finding}", file=sys.stderr)
        print(
            f"[uibus-boundary] FAIL: {len(findings)} D7 boundary violation(s) — editor.ui chrome "
            f"facts are editor-local and are never forwarded to the daemon.",
            file=sys.stderr,
        )
        return 1

    denied = ", ".join(literal for literal, _, _ in _DENIED_METHODS)
    print(
        f"[uibus-boundary] PASS: {BUS_MODULE} names no exit, no editor.ui subscription in "
        f"{root.as_posix()} reaches one, and no mirror-bearing module names a daemon-reaching "
        f"method ({denied})."
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
