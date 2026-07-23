#!/usr/bin/env python3
"""The NO-IDENTIFIERS gate for the e14d update check (M9 e14d, design 07 §4 / 08 threat row).

The design commitment is one line — *notify-only version GET, no identifiers, no telemetry anywhere*
— and the owner signed it personally. The C++ suite (`editor-shell-test_banners`) proves the half a
test can reach: the request VALUE the editor builds is a byte-for-byte constant carrying nothing
about the user or the machine. This scan proves the half a test cannot reach, because it lives BELOW
or BESIDE that value:

  1. **Is there exactly one place an outgoing request is built?**  Yes: `src/editor/shell/src/
     banners.cpp`. A second builder anywhere would be a second request shape, and the golden-equality
     assertion in the C++ suite would keep passing while it shipped — it only ever sees the first one.

  2. **Does the OS HTTPS client add anything of its own?**  It must not, and the three ways it would
     are all switched off in `native_net.cpp`: no default user agent (`WinHttpOpen` with a NULL
     agent), no synthesised accept types, and no ambient credentials (cookies + authentication
     disabled, autologon policy HIGH). Those are exactly the sort of line a later edit "tidies away",
     and no C++ assertion can observe what WinHTTP put on the wire. So they are pinned HERE. The same
     file may carry no header-name literal at all: every header on the wire comes from the request
     value or from nowhere.

  3. **Could editor-core make the request instead?**  It must not, and this is the subtle one. A
     renderer `fetch()` looks cleaner than a native HTTPS client and would be a PRIVACY REGRESSION:
     Chromium attaches its own `User-Agent` (OS, CPU, browser build), `Accept-Language` (the user's
     locale), and client hints beneath the JavaScript — identifiers no TypeScript assertion can see
     or remove. Any network primitive in editor-core is refused outright.

  4. **Is there a second endpoint hiding in the Shell?**  Only the two constants in `banners.h` (the
     release feed and the human downloads page) may appear as URLs in shipped Shell code. A telemetry
     or analytics host would otherwise be one string literal away.

Exit 0 = pass. Exit 1 = a violation (file, line, and what to do). Exit 2 = the scan could not run (a
bad --repo-root, or an anchor file that moved), which is deliberately NOT a pass: a gate that cannot
find its subject must say so rather than report OK.
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

# The ONE translation unit allowed to build an outgoing request, repo-root-relative.
SOLE_BUILDER = "src/editor/shell/src/banners.cpp"
# The header that DECLARES the request types + the two endpoint constants.
BUILDER_HEADER = "src/editor/shell/include/context/editor/shell/banners.h"
# The ONE translation unit that talks to the OS HTTPS client.
NATIVE_TRANSPORT = "src/editor/shell/src/native_net.cpp"

CPP_ROOTS = ("src",)
CPP_SUFFIXES = (".cpp", ".h", ".hpp", ".cc")

SHELL_ROOT = "src/editor/shell"
WEBUI_ROOT = "src/editor/webui"
TS_SUFFIXES = (".ts", ".tsx", ".mts", ".cts")

SKIP_DIRS = {"build", "build-msvc-check", "node_modules", "vcpkg_installed", ".git", "generated"}

# Building a request means naming its header type, naming the release endpoint constant, or MUTATING
# a request's header list.
#
# ⚠ THE `.headers` CLAUSE IS THE ONE THAT CATCHES THE REALISTIC SMUGGLE, and its absence let the first
# revision of this gate PASS a tree carrying
#
#     HttpRequest r = build_release_check_request();
#     r.headers.push_back({"X-Install-Id", machine_id()});   // brace-init: `HttpHeader` unnamed
#     (void)native_https_get(r);
#
# — a second request shape that names no header TYPE (C++20 brace-init needs none), no endpoint
# constant (it starts from the sanctioned builder) and no URL literal (rule 4 sees nothing). Found by
# PLANTING that shape, not by reading the regex; the plant is pinned in tools/tests/.
REQUEST_BUILD = re.compile(
    r"\bHttpHeader\s*\{|\bkReleaseCheckEndpoint\b|\bkReleaseCheckUserAgent\b"
    r"|\.headers\s*(?:\.\s*(?:push_back|emplace_back|emplace|insert|assign|resize)\s*\(|=[^=])"
)

# The OTHER half of that plant, and an independent net under it: the ONE route to the wire is
# `ReleaseNotice`, which issues `build_release_check_request()` through its bound transport. A direct
# CALL to the OS client anywhere else is a request nobody reviewed, whatever it carries. Binding the
# function as a value (`bind_transport(shell::native_https_get)` in the composition root) names no
# paren and is untouched; only an invocation matches.
TRANSPORT_CALL = re.compile(r"\bnative_https_get\s*\(")

# --- rule 2: what native_net.cpp MUST keep saying -------------------------------------------------
#
# Each entry is (pattern, why it is load-bearing). All must be present; the scan reads the file with
# comments STRIPPED, so a line that survives only inside a comment does not count as present.
REQUIRED_TRANSPORT_GUARDS = (
    (re.compile(r"WinHttpOpen\(\s*nullptr\s*,"),
     "WinHttpOpen must be called with a NULL agent, or WinHTTP contributes its own User-Agent"),
    (re.compile(r"\bWINHTTP_DISABLE_COOKIES\b"),
     "cookies must stay disabled — a cookie is an identifier the code never chose to send"),
    (re.compile(r"\bWINHTTP_DISABLE_AUTHENTICATION\b"),
     "authentication must stay disabled on an anonymous version query"),
    (re.compile(r"\bWINHTTP_AUTOLOGON_SECURITY_LEVEL_HIGH\b"),
     "the autologon policy must stay HIGH, or Windows may attach the signed-in user's credentials"),
    (re.compile(r"WinHttpOpenRequest\((?:[^;]*?)nullptr,\s*WINHTTP_NO_REFERER,"
                r"\s*WINHTTP_DEFAULT_ACCEPT_TYPES", re.DOTALL),
     "WinHttpOpenRequest must pass no version, no referrer and the default (NULL) accept types, "
     "or an `Accept: */*` is synthesised alongside the request's own"),
)

# --- rule 2 (negative): no header-name literal may live in the transport --------------------------
#
# Any of these spelled as a literal in `native_net.cpp` means a header is being contributed there
# rather than coming from the request value. The `X-` catch-all is deliberate: a custom header is the
# most natural place to smuggle an id.
#
# ⚠ MATCHING THE WHOLE LITERAL IS NOT ENOUGH, and that is how the first revision of this rule failed.
# `native_net.cpp`'s own idiom writes a header as `name + L": " + value + L"\r\n"`, so the natural
# smuggle carries the colon INSIDE the literal —
#
#     headers += L"X-Install-Id: ";
#     headers += widen(machine_sid());
#
# — which a `"…name…"`-anchored pattern refuses to match because the literal does not END at the
# name. The gate then PASSED a tree putting the machine SID on the wire: the precise leak rule 2
# exists to refuse, and one no C++ assertion can see, because it happens BELOW the request value the
# golden compares. Found by planting the shape production writes (conventions.md § Authoring a
# SOURCE-SCAN gate: "plant by COPYING an existing production call site"), not by reading the regex.
#
# So a literal is scanned as TEXT, and a header name counts when it is the whole literal OR sits in
# HTTP position — at the literal's start, or after a `\r\n` / `;` separator — followed by its colon.
# Requiring the colon is what keeps the prose-shaped names (`From`) out of the failure strings this
# file is full of.
HEADER_NAME = (r"[Uu]ser-[Aa]gent|[Aa]ccept-[Ll]anguage|[Aa]ccept-[Ee]ncoding|[Cc]ookie"
               r"|[Aa]uthorization|[Ff]rom|[Rr]eferer|[Rr]eferrer|[Xx]-[A-Za-z-]+")
# One C/C++ string literal, optionally wide, escapes respected.
STRING_LITERAL = re.compile(r'L?"(?:[^"\\\n]|\\.)*"')
TRANSPORT_HEADER_WHOLE = re.compile(rf"^(?:{HEADER_NAME})$")
TRANSPORT_HEADER_IN_POSITION = re.compile(rf"(?:^|\\r\\n|\\n|;)\s*(?:{HEADER_NAME})\s*:")


def transport_header_literals(text: str):
    """Yield (match_end, literal) for every string literal that names an HTTP header.

    `text` must already be comment-stripped: the file's own header block explains at length why there
    is no User-Agent here, and prose is not a contributed header.
    """
    for hit in STRING_LITERAL.finditer(text):
        body = hit.group(0)[hit.group(0).index('"') + 1:-1]
        if TRANSPORT_HEADER_WHOLE.match(body) or TRANSPORT_HEADER_IN_POSITION.search(body):
            yield hit.end(), hit.group(0)

# --- rule 3: network primitives editor-core may not use -------------------------------------------
TS_NETWORK = re.compile(
    r"\bfetch\s*\(|\bXMLHttpRequest\b|\bsendBeacon\s*\(|\bnew\s+WebSocket\b|\bEventSource\b"
    r"|\bnavigator\.connection\b"
)

# ...with ONE carve-out, and only for `fetch`: a SAME-ORIGIN relative path. It cannot name a host, so
# it cannot be an update check, an analytics beacon, or any other third-party destination — and it is
# how the T1 test bundle reports its verdict to its own local driver (`fetch("/report", …)` in
# `core/src/test/main.ts`, which `tools/webui_test_run.py` serves and blocks on).
#
# Deliberately NOT a blanket "tests are exempt" rule. The carve-out is a property of the ARGUMENT, so
# an absolute URL in a test is still a finding, and so is a `fetch(someVariable)` anywhere — the two
# shapes that could actually reach a remote host.
TS_FETCH_SAME_ORIGIN = re.compile(r"""\bfetch\s*\(\s*["'`](?:/|\./)""")

# --- rule 4: URL literals in shipped Shell code ---------------------------------------------------
#
# Requires at least one host character after the scheme, so the bare scheme-guard prefix
# `url.rfind("https://", 0)` in the transport is not a finding — it names no host.
URL_LITERAL = re.compile(r"https?://[A-Za-z0-9]")

# ⚠ THE `(?<!:)` IS LOAD-BEARING, and its absence silently disabled rule 4 in this file's first
# revision. A URL literal contains `//`, so a naive `//[^\n]*` comment regex blanks
# `"https://analytics.example.com/e"` from the `//` onward — leaving `"https:` behind, which matches
# no URL pattern. The gate then reported PASS on a tree with a planted analytics endpoint in it. Found
# by planting the violation, not by reading the regex; the lookbehind says "a `//` preceded by a colon
# starts no comment".
COMMENT = re.compile(r"(?<!:)//[^\n]*|/\*.*?\*/", re.DOTALL)


def strip_comments(text: str) -> str:
    """Blank comments (preserving newlines) so line numbers stay true and prose never trips a rule."""
    return COMMENT.sub(lambda m: re.sub(r"[^\n]", " ", m.group(0)), text)


def is_test_source(rel: str) -> bool:
    """Is this a test/fixture source rather than shipped product code?

    Rules 1 and 4 are about what SHIPS: the C++ suite necessarily names the endpoint constants (it
    PINS them) and a test fixture may hold a mock URL. Rules 2 and 3 do not use this at all — the
    transport has no test twin, and rule 3 carves out by the ARGUMENT's shape (a same-origin relative
    path) rather than by the file's role, so a test cannot reach a remote host either.
    """
    parts = rel.split("/")
    return "tests" in parts or "test" in parts or parts[-1].startswith("test_")


def iter_sources(root: Path, rel_roots: tuple[str, ...], suffixes: tuple[str, ...]):
    """Yield (relative_posix_path, comment-stripped text) for every scannable source."""
    for rel_root in rel_roots:
        base = root / rel_root
        if not base.is_dir():
            continue
        for path in sorted(base.rglob("*")):
            if path.suffix not in suffixes or not path.is_file():
                continue
            if any(part in SKIP_DIRS for part in path.relative_to(root).parts):
                continue
            try:
                text = path.read_text(encoding="utf-8", errors="replace")
            except OSError:
                continue
            yield path.relative_to(root).as_posix(), strip_comments(text)


def line_of(text: str, match_end: int) -> int:
    return text.count("\n", 0, match_end) + 1


def check(root: Path) -> list[str]:
    """Return the list of violations (empty = pass). Raises FileNotFoundError on a missing anchor."""
    for anchor in (SOLE_BUILDER, BUILDER_HEADER, NATIVE_TRANSPORT):
        if not (root / anchor).is_file():
            raise FileNotFoundError(
                f"{anchor} does not exist under {root} — the update-check anchor moved; update "
                f"tools/check_release_request.py to follow it (a gate that cannot find its subject "
                f"must fail, not pass)"
            )

    violations: list[str] = []

    # (1) exactly one request builder.
    for rel, text in iter_sources(root, CPP_ROOTS, CPP_SUFFIXES):
        if rel in (SOLE_BUILDER, BUILDER_HEADER) or is_test_source(rel):
            continue
        hit = REQUEST_BUILD.search(text)
        if hit is not None:
            violations.append(
                f"{rel}:{line_of(text, hit.end())}: builds an outgoing update-check request "
                f"({hit.group(0)!r}). There is exactly ONE builder ({SOLE_BUILDER}) so that "
                f"'what does the editor send' is answerable by reading one function — and so the "
                f"golden-request assertion in test_banners.cpp covers every request that exists."
            )
        # ...and the transport is reached only through `ReleaseNotice`. `native_net.cpp` DEFINES the
        # function, so it is exempt here; the composition root binds it as a value, which matches no
        # call pattern.
        if rel == NATIVE_TRANSPORT:
            continue
        call = TRANSPORT_CALL.search(text)
        if call is not None:
            violations.append(
                f"{rel}:{line_of(text, call.end())}: calls the OS HTTPS client directly. The ONE "
                f"route to the wire is `ReleaseNotice`, which issues "
                f"`build_release_check_request()` ({SOLE_BUILDER}) — a request sent from anywhere "
                f"else is one no assertion and no reviewer ever saw."
            )

    # (2) the OS transport keeps its guards and contributes no header of its own.
    transport = strip_comments((root / NATIVE_TRANSPORT).read_text(encoding="utf-8",
                                                                   errors="replace"))
    for pattern, why in REQUIRED_TRANSPORT_GUARDS:
        if pattern.search(transport) is None:
            violations.append(
                f"{NATIVE_TRANSPORT}: the guard `{pattern.pattern}` is gone — {why}. The OS client "
                f"must send EXACTLY the request value's headers and nothing else (banners.h "
                f"property (c)); nothing in C++ can observe a violation, which is why it is pinned "
                f"here."
            )
    for end, literal in transport_header_literals(transport):
        violations.append(
            f"{NATIVE_TRANSPORT}:{line_of(transport, end)}: names an HTTP header "
            f"({literal}). Headers come from the request VALUE built in {SOLE_BUILDER}, where "
            f"test_banners.cpp can see them — a header added here is invisible to every assertion."
        )

    # (3) editor-core makes no network call at all.
    for rel, text in iter_sources(root, (WEBUI_ROOT,), TS_SUFFIXES):
        for hit in TS_NETWORK.finditer(text):
            if TS_FETCH_SAME_ORIGIN.match(text, hit.start()) is not None:
                continue # a same-origin relative path names no host — see TS_FETCH_SAME_ORIGIN
            violations.append(
                f"{rel}:{line_of(text, hit.end())}: editor-core may not reach the network "
                f"({hit.group(0)!r}). A renderer request carries Chromium's own User-Agent, "
                f"Accept-Language and client hints beneath the JavaScript — identifiers the 08 "
                f"threat row forbids and no TypeScript assertion can see. The update check is a "
                f"Shell responsibility ({SOLE_BUILDER}); ask for its RESULT over the bridge."
            )

    # (4) no second endpoint in shipped Shell code.
    for rel, text in iter_sources(root, (SHELL_ROOT,), CPP_SUFFIXES):
        if rel == BUILDER_HEADER or is_test_source(rel):
            continue
        hit = URL_LITERAL.search(text)
        if hit is not None:
            violations.append(
                f"{rel}:{line_of(text, hit.end())}: names a URL directly. The Shell's only two "
                f"endpoints are the release feed and the downloads page, both constants in "
                f"{BUILDER_HEADER}; a third would be a network destination nobody reviewed."
            )

    return violations


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--repo-root", default=".", help="the Context-Engine repository root")
    args = ap.parse_args(argv)

    root = Path(args.repo_root).resolve()
    if not root.is_dir():
        print(f"[release-request] ERROR: --repo-root {root} is not a directory", file=sys.stderr)
        return 2
    try:
        violations = check(root)
    except (FileNotFoundError, OSError) as exc:
        print(f"[release-request] ERROR: {exc}", file=sys.stderr)
        return 2

    if violations:
        for v in violations:
            print(f"[release-request] FINDING: {v}", file=sys.stderr)
        print(
            f"[release-request] FAIL: {len(violations)} update-check privacy violation(s) — the "
            f"version GET carries NO identifiers (design 08, Update-check privacy)",
            file=sys.stderr,
        )
        return 1

    print("[release-request] PASS: one request builder, an OS transport that adds nothing, no "
          "network in editor-core, no second endpoint")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
