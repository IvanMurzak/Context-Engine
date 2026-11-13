"""Regenerate the README illustrations: py -3 docs/img/readme/build-svgs.py (run from anywhere).

svgkit.py next to this file is the shared visual style of our README images (copied from the workspace kit).
"""
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import svgkit as K  # noqa: E402


def out(name):
    return os.path.join(HERE, name)


# --- hero: the microkernel with packages around it ------------------------------------------------------
def kernel_visual():
    cx, cy = 970, 190
    parts = [K.mark(cx, cy, 64)]
    pkgs = [("physics", 0, -130), ("audio", 150, -64), ("input", 150, 64),
            ("ui", 0, 130), ("animation", -150, 64), ("particles", -150, -64)]
    for name, dx, dy in pkgs:
        w = round(len(name) * 9.8 + 32)
        s, _ = K.pill(round(cx + dx - w / 2), cy + dy - 17, name)
        parts.append(s)
    parts.append(f'<text x="{cx}" y="{cy + 186}" text-anchor="middle" class="s" font-size="16" fill="{K.MUTED}">'
                 'every feature is a package</text>')
    return "".join(parts)


K.write(out("hero.svg"), K.hero(
    title="Context ", accent="Engine", eyebrow_text="AI-first game engine · pre-alpha",
    subtitle=["Project files are the single source of truth.", "GUI, CLI and AI agents are equal clients."],
    chip_items=["C++20", "TypeScript", "WebGPU", "headless"],
    right=kernel_visual(), h=400,
    label="Context Engine — an AI-first, file-authoritative game engine (pre-alpha). Project files are the single "
          "source of truth; GUI, CLI and AI agents are equal clients. Every feature is a package on a microkernel."))

# --- architecture -----------------------------------------------------------------------------------------
body = "".join([
    K.eyebrow(40, 60, "Source of truth"),
    K.eyebrow(600, 60, "Headless daemon", anchor="middle"),
    K.eyebrow(1160, 60, "Equal clients", anchor="end"),
    K.card(40, 84, 290, 168, "Project files", "scenes, tilemaps: JSON", accent="yellow", sub2="gameplay: TypeScript"),
    K.card(440, 84, 320, 168, "EditorKernel", "live index over the files", accent="red", sub2="one contract: CLI · RPC · MCP"),
    K.card(870, 84, 290, 48, "Editor app", accent="blue"),
    K.card(870, 144, 290, 48, "context CLI", accent="blue"),
    K.card(870, 204, 290, 48, "AI agent", accent="blue"),
    K.wire("M330 148 H440", color=K.BLUE),
    K.wire("M440 188 H330", color=K.RED_TEXT, begin=1.2),
    K.wire("M760 168 C 815 168 815 108 870 108"),
    K.wire("M760 168 H870", begin=0.8),
    K.wire("M760 168 C 815 168 815 228 870 228", begin=1.6),
    K.wire("M520 252 V300", color=K.GREEN),
    K.card(40, 300, 550, 116, "RuntimeKernel", "fixed-tick sessions · state hash · replays", accent="green",
           sub2="TypeScript on V8 · C++/WASM native tier"),
    K.card(610, 300, 550, 116, "Renderer", "WebGPU: native and in the browser", accent="blue",
           sub2="2D is first-class"),
])
K.write(out("architecture.svg"), K.panel(1200, 450, body,
    "Architecture: project files are the source of truth. The headless EditorKernel daemon keeps a live index over "
    "them and serves one contract (CLI, RPC, MCP) to the editor app, the context CLI and AI agents. The RuntimeKernel "
    "runs deterministic fixed-tick sessions; the WebGPU renderer draws them natively and in the browser."))

# --- terminal: real commands; output abbreviated with … -------------------------------------------------
K.write(out("terminal.svg"), K.terminal(title="context — no GPU, no GUI", lines=[
    {"cmd": "context new my-game"},
    {"cmd": "context session new run.state --seed 7"},
    {"cmd": "context session step run.state --ticks 600 --json"},
    {"out": [("muted", '{"ok":'), ("ok", "true"), ("muted", ',"data":{"simTick":'), ("num", "600"),
             ("muted", ',"stateHash":{"root":'), ("str", '"…"'), ("muted", ',…}},…}')], "note": "600 fixed ticks"},
    {"cmd": "context session hash run.state --json"},
    {"out": [("muted", '{"ok":'), ("ok", "true"), ("muted", ',"data":{"simTick":'), ("num", "600"),
             ("muted", ',"stateHash":{"root":'), ("str", '"…"'), ("muted", ',…}},…}')], "note": "same seed + inputs, same hash"},
], label="Terminal: context new my-game; context session new run.state --seed 7; context session step run.state "
          "--ticks 600 --json returns an ok envelope with simTick 600 and the state hash; context session hash "
          "run.state --json returns the same state hash."))
