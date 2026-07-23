#!/usr/bin/env python3
"""Fail on any path by which an `editor.ui` chrome fact could reach the daemon (M9 e08c, D7).

Design 05 §5: the `editor.ui` bus is EDITOR-LOCAL and its facts are "NOT forwarded to the daemon
(D7)". The task that landed the bus required that boundary to be *proven, not stated* — and a runtime
test alone cannot prove it, because it only observes the code paths it happens to drive. This is the
static half: it sweeps EVERY editor-core source, so a forwarding path nothing drives is still caught.

Two rules, and both were verified by PLANTING a violation and watching this script go red.

RULE 1 — THE BUS MODULE IS STRUCTURALLY INCAPABLE OF REACHING THE DAEMON.
    editor-core's ONE way out is `ShellBridge` (src/bridge.ts): it wraps CEF's injected query
    function, and the Shell holds the daemon socket and the attach token behind it. `uibus.ts`
    therefore must not import that module, name that class, or otherwise mention the bridge — with no
    reference to the exit, no code in it can forward anything anywhere. It must also not INVOKE a
    `.call(`-shaped method, which closes the one route a name scan cannot see: a bridge handed in
    under an innocuous name.

RULE 2 — NO ui-TOPIC SUBSCRIPTION ANYWHERE REACHES THE EXIT.
    Rule 1 keeps the bus clean; it says nothing about a CONSUMER. `bus.subscribe("editor.ui.focus",
    (e) => void bridge.call(...))` in any other editor-core module would forward chrome facts out of
    the renderer while leaving uibus.ts pristine. So every `subscribe(` call whose topic argument
    names a ui topic — a `UI_TOPIC_*` constant or an `editor.ui.*` literal — has its callback text
    scanned for the exit, and a hit is a FINDING.

WHAT THIS DELIBERATELY DOES NOT FORBID: the cross-window MIRROR seam (`UiMirrorSink`). Mirroring
chrome facts between the Shell's own windows is the design (05 §5), and e10's sink will legitimately
call a SHELL-local bridge method. That sink is not a subscription and is not matched here. When a
daemon-FORWARDING bridge method ever exists (there is none today — every method the Shell routes is
Shell-local: `panel.*`, `themes.get`, `config.*`, `keybindings.get`, `editor.state.*`, `welcome.*`),
this gate should grow a deny-list naming it; until then a mirror sink cannot reach the daemon because
no such method exists to reach it through.

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

# The subscribe call site. The OPTIONAL TYPE ARGUMENT is load-bearing: editor-core's own real call is
# `bus.subscribe<ThemeChangedPayload>(UI_TOPIC_THEME_CHANGED, …)`, and a bare `subscribe\s*\(` pattern
# silently skipped every generic one — which is how this gate passed a PLANTED forwarding path on its
# first run. Found by planting a violation, not by reading the regex.
_SUBSCRIBE = re.compile(r"\bsubscribe\s*(?:<[^<>()]*>)?\s*\(")

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


def check_bus_module(source: str) -> list[str]:
    """RULE 1: the bus module must not name — or invoke — editor-core's exit (comments excluded)."""
    findings: list[str] = []
    code = _strip_comments(source)
    for number, line in enumerate(code.splitlines(), start=1):
        if _BUS_EXIT.search(line):
            findings.append(
                f"{BUS_MODULE}:{number}: the editor.ui bus names editor-core's ONE exit "
                f"({line.strip()}) — D7 requires it to be structurally incapable of reaching the "
                f"daemon (design 05 §5)"
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
        hit = _EXIT.search(callback)
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
    """Run both rules over `source_root`. Raises FileNotFoundError when the bus module is missing."""
    bus = source_root / BUS_MODULE
    if not bus.is_file():
        raise FileNotFoundError(f"{bus} does not exist")
    findings = check_bus_module(bus.read_text(encoding="utf-8"))
    for path in sorted(source_root.rglob("*")):
        if not path.is_file() or path.suffix not in _TS_EXTENSIONS or _is_test_source(path):
            continue
        findings.extend(
            check_subscriptions(path.relative_to(source_root).as_posix(),
                                path.read_text(encoding="utf-8"))
        )
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

    print(
        f"[uibus-boundary] PASS: {BUS_MODULE} names no exit, and no editor.ui subscription in "
        f"{root.as_posix()} reaches one."
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
