"""Tests for tools/check_release_request.py — the e14d update-check privacy gate (R-QA-013).

MUTATION COVERAGE, not happy-path coverage. A gate whose only test is "the live tree passes" is
indistinguishable from a gate that always passes, and this gate stands behind a privacy commitment
the owner signed personally (08 threat row: notify-only version GET, no identifiers). So every rule
is exercised by PLANTING the violation it exists to catch into a synthetic tree — and the live
repository is then checked once at the end, where it must pass with its anchors where the tool
believes they are.
"""

from __future__ import annotations

from pathlib import Path

import pytest
from conftest import load_tool

check_release_request = load_tool("check_release_request")

REPO_ROOT = Path(__file__).resolve().parents[2]

# A transport TU carrying every guard the gate requires. Kept as the fixture's baseline so a test can
# remove ONE guard and prove the gate notices that one.
CLEAN_TRANSPORT = """
HttpResponse native_https_get(const HttpRequest& request)
{
    WinHttpHandle session(::WinHttpOpen(nullptr, WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                        WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0));
    WinHttpHandle call(::WinHttpOpenRequest(connection.get(), method.c_str(), path.c_str(), nullptr,
                                            WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                            WINHTTP_FLAG_SECURE));
    DWORD disabled = WINHTTP_DISABLE_COOKIES | WINHTTP_DISABLE_AUTHENTICATION;
    DWORD autologon = WINHTTP_AUTOLOGON_SECURITY_LEVEL_HIGH;
    for (const HttpHeader& header : request.headers) { headers += widen(header.name); }
    return response;
}
"""


def make_tree(tmp_path: Path) -> Path:
    """A minimal tree carrying the three anchors the tool requires, and nothing else."""
    builder = tmp_path / check_release_request.SOLE_BUILDER
    builder.parent.mkdir(parents=True, exist_ok=True)
    builder.write_text(
        "HttpRequest build_release_check_request()\n"
        "{\n"
        "    HttpRequest request;\n"
        "    request.url = kReleaseCheckEndpoint;\n"
        '    request.headers.push_back(HttpHeader{"Accept", kReleaseCheckAccept});\n'
        "    return request;\n"
        "}\n",
        encoding="utf-8",
    )
    header = tmp_path / check_release_request.BUILDER_HEADER
    header.parent.mkdir(parents=True, exist_ok=True)
    header.write_text(
        'inline constexpr const char* kReleaseCheckEndpoint = "https://example.invalid/latest";\n'
        'inline constexpr const char* kReleaseCheckUserAgent = "Context-Editor";\n',
        encoding="utf-8",
    )
    transport = tmp_path / check_release_request.NATIVE_TRANSPORT
    transport.parent.mkdir(parents=True, exist_ok=True)
    transport.write_text(CLEAN_TRANSPORT, encoding="utf-8")
    return tmp_path


def test_clean_tree_passes(tmp_path: Path) -> None:
    assert check_release_request.check(make_tree(tmp_path)) == []


def test_missing_anchor_is_an_error_not_a_pass(tmp_path: Path) -> None:
    with pytest.raises(FileNotFoundError):
        check_release_request.check(tmp_path)
    assert check_release_request.main(["--repo-root", str(tmp_path)]) == 2


# --- rule 1: exactly one request builder ----------------------------------------------------------

@pytest.mark.parametrize(
    "planted",
    [
        'auto h = HttpHeader{"X-Install-Id", machine_id()};',   # a second builder, by header type
        "request.url = kReleaseCheckEndpoint;",                  # ...or by naming the endpoint
        'headers.push_back({"User-Agent", kReleaseCheckUserAgent});',
    ],
)
def test_a_second_request_builder_is_refused(tmp_path: Path, planted: str) -> None:
    root = make_tree(tmp_path)
    other = root / "src/editor/shell/src/other_surface.cpp"
    other.write_text(f"void f() {{ {planted} }}\n", encoding="utf-8")
    violations = check_release_request.check(root)
    assert any("other_surface.cpp" in v and "builds an outgoing" in v for v in violations)


@pytest.mark.parametrize(
    "planted",
    [
        # The realistic smuggle: start from the SANCTIONED request and add one header by brace-init,
        # which under C++20 names no `HttpHeader` type at all.
        'auto r = build_release_check_request(); r.headers.push_back({"X-Install-Id", id()});',
        'r.headers.emplace_back("X-Install-Id", id());',
        "r.headers = collect_host_headers();",
        # ...and the second net under it: reaching the OS client from anywhere but ReleaseNotice.
        "(void)native_https_get(r);",
    ],
)
def test_mutating_the_sanctioned_request_is_a_second_builder(tmp_path: Path, planted: str) -> None:
    """The regression test for the FIRST real defect planting found in this gate.

    A tree carrying `build_release_check_request()` + `r.headers.push_back({"X-Install-Id", …})` +
    `native_https_get(r)` in the composition root named no header TYPE (brace-init), no endpoint
    constant (it starts from the sanctioned builder) and no URL literal — so it evaded ALL FOUR rules
    and the gate reported PASS on a tree that put a machine id on the wire. Found by planting, per
    conventions.md § Authoring a SOURCE-SCAN gate (2): plant the shape production would write, not a
    synthetic minimal repro.
    """
    root = make_tree(tmp_path)
    other = root / "src/editor/shell/app/editor_main.cpp"
    other.parent.mkdir(parents=True, exist_ok=True)
    other.write_text(f"int main() {{ {planted} }}\n", encoding="utf-8")
    assert check_release_request.check(root) != []


def test_binding_the_transport_as_a_value_is_not_a_call(tmp_path: Path) -> None:
    """The composition root MUST stay able to hand the OS client to `ReleaseNotice`.

    `bind_transport(shell::native_https_get)` names the function without invoking it, so the
    direct-call rule above must not fire on the one wiring line the design requires.
    """
    root = make_tree(tmp_path)
    other = root / "src/editor/shell/app/editor_main.cpp"
    other.parent.mkdir(parents=True, exist_ok=True)
    other.write_text(
        "int main() { notice.bind_transport(shell::native_https_get);"
        " notice.bind_url_opener(shell::native_open_url); }\n",
        encoding="utf-8",
    )
    assert check_release_request.check(root) == []


def test_the_sole_builder_and_a_test_are_not_second_builders(tmp_path: Path) -> None:
    root = make_tree(tmp_path)
    suite = root / "src/editor/shell/tests/test_banners.cpp"
    suite.parent.mkdir(parents=True, exist_ok=True)
    # The C++ suite necessarily PINS the constants — pinning is the opposite of a second builder.
    suite.write_text("CHECK(request.url == kReleaseCheckEndpoint);\n", encoding="utf-8")
    assert check_release_request.check(root) == []


# --- rule 2: the OS transport adds nothing --------------------------------------------------------

@pytest.mark.parametrize(
    "removed",
    [
        "WINHTTP_DISABLE_COOKIES",
        "WINHTTP_DISABLE_AUTHENTICATION",
        "WINHTTP_AUTOLOGON_SECURITY_LEVEL_HIGH",
        "WINHTTP_DEFAULT_ACCEPT_TYPES",
    ],
)
def test_dropping_a_transport_guard_is_refused(tmp_path: Path, removed: str) -> None:
    root = make_tree(tmp_path)
    path = root / check_release_request.NATIVE_TRANSPORT
    path.write_text(CLEAN_TRANSPORT.replace(removed, "0"), encoding="utf-8")
    violations = check_release_request.check(root)
    assert any("the guard" in v for v in violations)


def test_a_non_null_user_agent_on_winhttpopen_is_refused(tmp_path: Path) -> None:
    root = make_tree(tmp_path)
    path = root / check_release_request.NATIVE_TRANSPORT
    # The single most likely regression: someone "fixes" the missing agent by naming one here, where
    # no C++ assertion can see it.
    path.write_text(CLEAN_TRANSPORT.replace("::WinHttpOpen(nullptr,",
                                            '::WinHttpOpen(L"Context-Editor/1.0",'),
                    encoding="utf-8")
    violations = check_release_request.check(root)
    assert any("WinHttpOpen" in v for v in violations)


@pytest.mark.parametrize(
    "literal",
    ['L"User-Agent"', 'L"Accept-Language"', '"Cookie"', 'L"X-Install-Id"', '"Authorization"'],
)
def test_a_header_literal_in_the_transport_is_refused(tmp_path: Path, literal: str) -> None:
    root = make_tree(tmp_path)
    path = root / check_release_request.NATIVE_TRANSPORT
    path.write_text(CLEAN_TRANSPORT + f"\nvoid extra() {{ add({literal}); }}\n", encoding="utf-8")
    violations = check_release_request.check(root)
    assert any("names an HTTP header" in v for v in violations)


@pytest.mark.parametrize(
    "planted",
    [
        'headers += L"X-Install-Id: ";',            # the shape this file's own idiom writes
        'headers += L"X-Install-Id: " + widen(id);',
        'blob = L"Accept-Language: en-GB\\r\\n";',
        'add(L"User-Agent: Context-Editor/1.4.2 (Windows 11; x64)\\r\\n");',
        'add(L"Cookie: install=abc\\r\\nFrom: user@host\\r\\n");',  # a name after a \\r\\n separator
    ],
)
def test_a_header_smuggled_in_http_position_is_refused(tmp_path: Path, planted: str) -> None:
    """The regression test for the second real defect planting found in this gate.

    The first revision matched a header name only when it was the ENTIRE literal, so it PASSED a tree
    appending `L"X-Install-Id: "` — with the colon inside the literal — straight onto the wire. That
    is precisely the shape `native_net.cpp` writes headers in (`name + L": " + value`), it is
    invisible to every C++ assertion (it happens BELOW the request value the golden compares), and it
    is the leak rule 2 exists to refuse. Found by planting the production shape, not by reading the
    regex — conventions.md § Authoring a SOURCE-SCAN gate (2).
    """
    root = make_tree(tmp_path)
    path = root / check_release_request.NATIVE_TRANSPORT
    path.write_text(CLEAN_TRANSPORT + f"\nvoid extra() {{ {planted} }}\n", encoding="utf-8")
    violations = check_release_request.check(root)
    assert any("names an HTTP header" in v for v in violations)


def test_transport_prose_without_a_header_colon_is_not_a_header(tmp_path: Path) -> None:
    """The transport is full of human failure strings; a bare English word is not a contributed header.

    Requiring the header's COLON is what separates the two. (A literal that genuinely reads
    `"...from: ..."` would be reported — a false positive the gate accepts, since rewording an error
    string is cheap and a missed identifier is not.)
    """
    root = make_tree(tmp_path)
    path = root / check_release_request.NATIVE_TRANSPORT
    path.write_text(
        CLEAN_TRANSPORT
        + '\nvoid f() { return failure("the release feed response could not be read from the '
          'socket"); }\n',
        encoding="utf-8",
    )
    assert check_release_request.check(root) == []


def test_a_comment_in_the_transport_is_not_a_header(tmp_path: Path) -> None:
    root = make_tree(tmp_path)
    path = root / check_release_request.NATIVE_TRANSPORT
    # The real file's own header block explains WHY there is no User-Agent here. Prose is not code.
    path.write_text('// no L"User-Agent" is contributed here\n' + CLEAN_TRANSPORT, encoding="utf-8")
    assert check_release_request.check(root) == []


# --- rule 3: editor-core reaches no network -------------------------------------------------------

@pytest.mark.parametrize(
    "planted",
    [
        'await fetch("https://api.example.com/latest");',
        "const x = new XMLHttpRequest();",
        'navigator.sendBeacon("/x", body);',
        'const s = new WebSocket("wss://example.com");',
        "const e = new EventSource(url);",
        "await fetch(endpointFromConfig);",  # a variable argument could be any host
    ],
)
def test_a_network_call_in_editor_core_is_refused(tmp_path: Path, planted: str) -> None:
    root = make_tree(tmp_path)
    module = root / "src/editor/webui/core/src/updates.ts"
    module.parent.mkdir(parents=True, exist_ok=True)
    module.write_text(f"export async function check() {{ {planted} }}\n", encoding="utf-8")
    violations = check_release_request.check(root)
    assert any("may not reach the network" in v for v in violations)


def test_a_same_origin_relative_fetch_is_allowed(tmp_path: Path) -> None:
    root = make_tree(tmp_path)
    module = root / "src/editor/webui/core/src/test/main.ts"
    module.parent.mkdir(parents=True, exist_ok=True)
    # The T1 harness reporting its verdict to its own local driver. It names no host, so it cannot be
    # an update check or a beacon.
    module.write_text('void fetch("/report", { method: "POST", body });\n', encoding="utf-8")
    assert check_release_request.check(root) == []


def test_an_absolute_fetch_in_a_test_is_still_refused(tmp_path: Path) -> None:
    root = make_tree(tmp_path)
    module = root / "src/editor/webui/core/src/test/sneaky.test.ts"
    module.parent.mkdir(parents=True, exist_ok=True)
    module.write_text('void fetch("https://telemetry.example.com/ping");\n', encoding="utf-8")
    violations = check_release_request.check(root)
    assert any("may not reach the network" in v for v in violations)


# --- rule 4: no second endpoint in shipped Shell code ---------------------------------------------

def test_a_second_endpoint_in_the_shell_is_refused(tmp_path: Path) -> None:
    root = make_tree(tmp_path)
    module = root / "src/editor/shell/src/metrics.cpp"
    module.write_text('void ping() { post("https://analytics.example.com/e"); }\n', encoding="utf-8")
    violations = check_release_request.check(root)
    assert any("names a URL directly" in v for v in violations)


def test_a_url_is_not_mistaken_for_a_comment(tmp_path: Path) -> None:
    """A URL contains `//`. The first revision's comment regex blanked it and rule 4 never fired.

    This is the regression test for that defect: the planted endpoint must be found even though it is
    the ONLY thing on its line and its `//` looks exactly like a line comment.
    """
    root = make_tree(tmp_path)
    module = root / "src/editor/shell/src/metrics.cpp"
    module.write_text('const char* kPing = "https://analytics.example.com/e";\n', encoding="utf-8")
    assert any("names a URL directly" in v for v in check_release_request.check(root))

    # ...and a genuine comment mentioning a URL is still stripped, so prose cannot trip the rule.
    module.write_text("// see https://analytics.example.com/e for why we do NOT ping it\n",
                      encoding="utf-8")
    assert check_release_request.check(root) == []


def test_the_bare_https_scheme_guard_is_not_an_endpoint(tmp_path: Path) -> None:
    root = make_tree(tmp_path)
    path = root / check_release_request.NATIVE_TRANSPORT
    # `native_open_url` refuses anything that is not https by testing this prefix. It names no host.
    path.write_text(CLEAN_TRANSPORT + '\nbool ok(const std::string& u)'
                                      ' { return u.rfind("https://", 0) == 0; }\n',
                    encoding="utf-8")
    assert check_release_request.check(root) == []


# --- the live tree --------------------------------------------------------------------------------

def test_the_live_repository_passes() -> None:
    assert check_release_request.check(REPO_ROOT) == []
    assert check_release_request.main(["--repo-root", str(REPO_ROOT)]) == 0
