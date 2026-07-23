# src/editor/webui/ — the M9 editor-core npm workspace + its build-time toolchain (e05a, design 04
# section 1 / 05 section 3 / 08 section 3).
#
# This is the repo's FIRST build-time BUNDLING target. Everything here is a build step over
# SHA-pinned, fetch-verified tooling — there is no `npm install`, no Node at runtime, and no npm in
# CI beyond the pinned channels (the standing repo convention). Three pinned tools cooperate:
#
#   esbuild (tools/ts-toolchain.json)      — bundles the workspace into a static asset
#   tsgo    (tools/tsc-toolchain.json)     — SEMANTIC typecheck; esbuild strips types without
#                                            checking them, so this is the only type gate
#   dockview-core (tools/dockview-toolchain.json) — the docking engine, staged beside the bundle
#
# e05a stopped at the SUBSTRATE: the workspace type-checked and bundled, and the docking engine was
# acquired + verified, but nothing consumed it and nothing SERVED it. e05c added the served document
# set (index.html + app.css) and published the asset root the Shell's context-editor:// resource
# handler reads. e05d1 adds the APP — PanelHost over Dockview and the hydration runtime — so the
# docking engine is now consumed through its API.
#
# NOTE ON HOW DOCKVIEW REACHES THE RENDERER, because it is NOT bundled: `dockview-core.min.js` is a
# UMD artifact staged beside the bundle and loaded by `index.html` as a plain same-origin <script>,
# which publishes `globalThis["dockview-core"]`. Keeping it out of the esbuild bundle is deliberate —
# the shipped bytes stay byte-identical to the artifact fetch_dockview.py SHA-verified, which is what
# the `webui-assets` re-hash below actually checks. Bundling it would quietly make that check verify
# a file nothing loads.
#
# Layout persistence + region maps are e05d2's; the live Scene tree / Inspector are e05d3's — keep
# this directory's growth on those seams.

find_package(Python3 COMPONENTS Interpreter REQUIRED)

# --- the pinned tool paths (the src/runtime/ts ordering constraint) -----------------------------
# CONTEXT_ESBUILD_BIN / CONTEXT_TSGO_BIN are published as CACHE INTERNAL by src/runtime/ts, which
# MUST therefore be configured BEFORE this directory (src/CMakeLists.txt orders it so). An empty
# value here is a build-graph regression, not a user error: fail LOUDLY at configure time rather
# than emitting a custom command whose program is the empty string (which fails much later with a
# message that points nowhere near the cause).
if(NOT CONTEXT_ESBUILD_BIN)
    message(FATAL_ERROR
        "context_editor_webui: CONTEXT_ESBUILD_BIN is empty — src/editor/webui must be added AFTER "
        "add_subdirectory(runtime/ts), which stages + publishes the pinned esbuild.")
endif()
if(NOT CONTEXT_TSGO_BIN)
    message(FATAL_ERROR
        "context_editor_webui: CONTEXT_TSGO_BIN is empty — src/editor/webui must be added AFTER "
        "add_subdirectory(runtime/ts), which stages + publishes the pinned tsgo typechecker.")
endif()
# The generated client schema (e02) is this workspace's codegen input; same ordering logic.
if(NOT CONTEXT_CLIENT_SCHEMA_GENERATED)
    message(FATAL_ERROR
        "context_editor_webui: CONTEXT_CLIENT_SCHEMA_GENERATED is empty — src/editor/webui must be "
        "added AFTER add_subdirectory(editor/client), which generates the client schema.")
endif()

set(_ctx_webui_core "${CMAKE_CURRENT_SOURCE_DIR}/core")
# The @context-engine/editor-tokens data-layer package (M9 e06a, design 06 / D11). Its schema +
# validator TS and the built-in *.theme.json / theme.schema.json are imported by the editor-core T1
# test entry (core/src/test/tokens.test.ts), so they are typecheck + bundle inputs of the test bundle.
# Listing them below keeps an incremental build correct when a token source changes (a clean CI build
# rebuilds regardless). The tokens package declares NO npm dependency (its package.json has none), so
# it does not touch the dockview-core-only web-layer allowlist.
set(_ctx_webui_tokens "${CMAKE_CURRENT_SOURCE_DIR}/tokens")
# The @context-engine/editor-kit component kit (M9 e06c1, design 06 section 3 / 04 section 4). Its
# module is imported by core/src/hydration.ts (which re-exports WIDGET_CLASSES), so it is a typecheck
# + bundle input exactly like tokens/; its stylesheet is STAGED into the served asset root below.
set(_ctx_webui_kit "${CMAKE_CURRENT_SOURCE_DIR}/kit")
set(_ctx_webui_tsconfig "${_ctx_webui_core}/tsconfig.json")
set(_ctx_webui_entry "${_ctx_webui_core}/src/index.ts")
# The asset root the Shell will serve over context-editor://app/ (e05c wires the scheme).
set(_ctx_webui_assets "${CMAKE_BINARY_DIR}/editor-webui/app")
# Declared once and composed forward, so the asset gate below names the same file the bundle step
# writes without either side restating the literal.
set(_ctx_webui_bundle_name "editor-core.js")
set(_ctx_webui_bundle "${_ctx_webui_assets}/${_ctx_webui_bundle_name}")
set(_ctx_webui_stamps "${CMAKE_CURRENT_BINARY_DIR}/stamps")

# --- acquire + verify the pinned dockview-core distribution (verify-before-use, fail-closed) -----
# Configure-time, mirroring how src/runtime/ts stages esbuild/tsgo: the assets must exist before the
# build graph copies them, and a supply-chain refusal should stop the build at CONFIGURE, as early
# and as loudly as possible.
set(_ctx_dockview_manifest "${CMAKE_SOURCE_DIR}/../tools/dockview-toolchain.json")
set(_ctx_dockview_fetch "${CMAKE_SOURCE_DIR}/../tools/fetch_dockview.py")
set(_ctx_dockview_dest "${CMAKE_BINARY_DIR}/_dockview")
message(STATUS "context_editor_webui: fetching + verifying pinned dockview-core")
execute_process(
    COMMAND "${Python3_EXECUTABLE}" "${_ctx_dockview_fetch}"
            --manifest "${_ctx_dockview_manifest}"
            --dest "${_ctx_dockview_dest}"
    RESULT_VARIABLE _ctx_dockview_rc
    OUTPUT_VARIABLE _ctx_dockview_out
    ERROR_VARIABLE _ctx_dockview_err)
if(NOT _ctx_dockview_rc EQUAL 0)
    message(FATAL_ERROR
        "context_editor_webui: dockview-core fetch/verify FAILED (rc=${_ctx_dockview_rc}) — refusing "
        "to build against an unverified docking engine (verify-before-use fail-closed).\n"
        "stdout: ${_ctx_dockview_out}\nstderr: ${_ctx_dockview_err}")
endif()
string(STRIP "${_ctx_dockview_out}" _ctx_dockview_out)
message(STATUS "context_editor_webui: ${_ctx_dockview_out}")

# The member names the manifest stages, READ FROM the manifest rather than mirrored here: the pins
# already live in exactly one place (tools/dockview-toolchain.json), and a hand-kept copy of the
# names would drift into a failure far from the edit that caused it.
# (No empty-`members` guard needed here: the fetch above already ran check_manifest(), which refuses
# a missing/empty member table, and a non-zero rc FATAL_ERRORs twenty lines up — so reaching this
# point means `members` is a non-empty object.)
file(READ "${_ctx_dockview_manifest}" _ctx_dockview_json)
string(JSON _ctx_dockview_member_count LENGTH "${_ctx_dockview_json}" members)
math(EXPR _ctx_dockview_last_member "${_ctx_dockview_member_count} - 1")
set(_ctx_dockview_members "")
foreach(_ctx_dockview_i RANGE ${_ctx_dockview_last_member})
    string(JSON _ctx_dockview_member MEMBER "${_ctx_dockview_json}" members ${_ctx_dockview_i})
    list(APPEND _ctx_dockview_members "${_ctx_dockview_member}")
endforeach()

# --- workspace sources ---------------------------------------------------------------------------
# CONFIGURE_DEPENDS so adding a source file re-globs on the next build instead of silently not being
# typechecked (a stale-glob trap that would make the type gate quietly incomplete). Every extension
# the typechecker accepts is globbed, not just `.ts`: a `.tsx` panel — e05c's most likely shape — that
# neither this glob nor tsconfig's `include` covers would be silently exempt from the type gate AND
# absent from the bundle's DEPENDS, narrowing the gate exactly when it starts mattering. tsconfig's
# `include` is `src/**/*`, which TypeScript resolves to this same extension set, so the two lists
# cannot drift apart.
file(GLOB_RECURSE _ctx_webui_ts CONFIGURE_DEPENDS
     "${_ctx_webui_core}/src/*.ts" "${_ctx_webui_core}/src/*.tsx"
     "${_ctx_webui_core}/src/*.mts" "${_ctx_webui_core}/src/*.cts")
# The editor-tokens data layer (M9 e06a) — the schema/validator TS plus the built-in theme DATA the
# T1 test entry imports directly (esbuild inlines the *.theme.json via its json loader; tsgo resolves
# them via resolveJsonModule). CONFIGURE_DEPENDS so a new theme or a schema edit re-globs.
file(GLOB_RECURSE _ctx_webui_tokens_ts CONFIGURE_DEPENDS "${_ctx_webui_tokens}/src/*.ts")
file(GLOB _ctx_webui_tokens_data CONFIGURE_DEPENDS
     "${_ctx_webui_tokens}/themes/*.theme.json" "${_ctx_webui_tokens}/theme.schema.json")
# The component kit's TS (M9 e06c1). CONFIGURE_DEPENDS for the same reason as the core glob: a kit
# module added without re-globbing would be silently exempt from the type gate AND absent from the
# bundle's DEPENDS — the exact stale-glob trap, one package over. Its STYLESHEETS are listed too, so
# an incremental build re-stages `kit.css` when it changes (and re-runs the ctests that read it).
file(GLOB_RECURSE _ctx_webui_kit_ts CONFIGURE_DEPENDS
     "${_ctx_webui_kit}/src/*.ts" "${_ctx_webui_kit}/src/*.tsx"
     "${_ctx_webui_kit}/src/*.mts" "${_ctx_webui_kit}/src/*.cts")
file(GLOB_RECURSE _ctx_webui_kit_styles CONFIGURE_DEPENDS "${_ctx_webui_kit}/styles/*.css")
set(_ctx_webui_inputs ${_ctx_webui_ts} ${_ctx_webui_tokens_ts} ${_ctx_webui_tokens_data}
    ${_ctx_webui_kit_ts}
    "${_ctx_webui_tsconfig}" "${_ctx_webui_core}/package.json" "${_ctx_webui_tokens}/package.json"
    "${_ctx_webui_kit}/package.json")

# --- 1) SEMANTIC typecheck (a BUILD failure, per the e05a DoD) -----------------------------------
# `--pretty false` keeps diagnostics machine-legible in CI logs (the same flag src/runtime/ts's
# tsgo driver passes). The stamp is what the bundle depends on, which is what makes the typecheck
# GATE the bundle rather than merely run beside it.
add_custom_command(
    OUTPUT "${_ctx_webui_stamps}/typecheck.stamp"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${_ctx_webui_stamps}"
    COMMAND "${CONTEXT_TSGO_BIN}" -p "${_ctx_webui_tsconfig}" --noEmit --pretty false
    COMMAND "${CMAKE_COMMAND}" -E touch "${_ctx_webui_stamps}/typecheck.stamp"
    WORKING_DIRECTORY "${_ctx_webui_core}"
    DEPENDS ${_ctx_webui_inputs}
    COMMENT "context_editor_webui: typechecking @context-engine/editor-core (pinned tsgo --noEmit)"
    VERBATIM)

# --- 2) bundle through the pinned esbuild --------------------------------------------------------
# format=esm + platform=browser + target=es2022 matches the trusted editor-core zone: a module the
# CEF renderer loads directly, with no CommonJS/Node interop shim.
add_custom_command(
    OUTPUT "${_ctx_webui_bundle}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${_ctx_webui_assets}"
    COMMAND "${CONTEXT_ESBUILD_BIN}" "${_ctx_webui_entry}"
            --bundle
            --format=esm
            --platform=browser
            --target=es2022
            "--outfile=${_ctx_webui_bundle}"
    DEPENDS ${_ctx_webui_inputs} "${_ctx_webui_stamps}/typecheck.stamp"
    COMMENT "context_editor_webui: bundling @context-engine/editor-core (pinned esbuild)"
    VERBATIM)

# --- 3) stage the verified dockview assets beside the bundle -------------------------------------
# They are part of the served asset set, not build scratch. copy_if_different keeps incremental
# builds quiet; the webui-assets ctest re-hashes them against the pin afterwards.
set(_ctx_dockview_staged "")
foreach(_member IN LISTS _ctx_dockview_members)
    add_custom_command(
        OUTPUT "${_ctx_webui_assets}/${_member}"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${_ctx_webui_assets}"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                "${_ctx_dockview_dest}/${_member}" "${_ctx_webui_assets}/${_member}"
        DEPENDS "${_ctx_dockview_dest}/${_member}"
        COMMENT "context_editor_webui: staging pinned ${_member}"
        VERBATIM)
    list(APPEND _ctx_dockview_staged "${_ctx_webui_assets}/${_member}")
endforeach()

# --- 4) stage the served document set (e05c) -----------------------------------------------------
# index.html + app.css are SOURCE files, not generated ones: they are hand-authored, CSP-constrained
# documents (no inline script, no inline style — see the comment blocks inside them), so they are
# copied rather than emitted. They live beside the bundle because the resource handler serves ONE
# flat asset root over `context-editor://app/`.
set(_ctx_webui_documents index.html app.css)
set(_ctx_webui_staged_documents "")
foreach(_doc IN LISTS _ctx_webui_documents)
    add_custom_command(
        OUTPUT "${_ctx_webui_assets}/${_doc}"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${_ctx_webui_assets}"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                "${CMAKE_CURRENT_SOURCE_DIR}/app/${_doc}" "${_ctx_webui_assets}/${_doc}"
        DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/app/${_doc}"
        COMMENT "context_editor_webui: staging ${_doc}"
        VERBATIM)
    list(APPEND _ctx_webui_staged_documents "${_ctx_webui_assets}/${_doc}")
endforeach()

# --- 4b) stage the component KIT's stylesheet (e06c1) --------------------------------------------
# Served as `context-editor://app/kit.css`, beside app.css and linked by index.html. It comes from a
# DIFFERENT package (kit/, not app/), which is why it is staged here rather than added to the loop
# above — the loop's source dir is app/ and widening it to two roots would obscure which document
# belongs to which package.
#
# The file name is spelled ONCE more, in the kit's own `KIT_STYLESHEET` constant, because the T1 tier
# looks the sheet up by name in `document.styleSheets` (a test that cannot find it must fail loudly,
# not pass vacuously). A rename here without renaming there turns `kit.test.ts` red immediately, which
# is the intended coupling.
set(_ctx_webui_kit_stylesheet_name "kit.css")
add_custom_command(
    OUTPUT "${_ctx_webui_assets}/${_ctx_webui_kit_stylesheet_name}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${_ctx_webui_assets}"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${_ctx_webui_kit}/styles/${_ctx_webui_kit_stylesheet_name}"
            "${_ctx_webui_assets}/${_ctx_webui_kit_stylesheet_name}"
    DEPENDS "${_ctx_webui_kit}/styles/${_ctx_webui_kit_stylesheet_name}"
    COMMENT "context_editor_webui: staging the component kit stylesheet"
    VERBATIM)
list(APPEND _ctx_webui_staged_documents "${_ctx_webui_assets}/${_ctx_webui_kit_stylesheet_name}")

# ALL so a plain `cmake --build` produces the asset set — the bundle is a build output on every leg,
# exactly like editor/client's generated schema.
add_custom_target(context_editor_webui ALL
    DEPENDS "${_ctx_webui_bundle}" ${_ctx_dockview_staged} ${_ctx_webui_staged_documents})

# --- publish the asset root for the Shell (e05c) --------------------------------------------------
# CACHE INTERNAL, not a plain variable: a plain `set()` is scoped to this directory and would read
# EMPTY in src/editor/shell/, exactly the toolchain-seam trap CONTEXT_ESBUILD_BIN documents at the
# top of this file. src/CMakeLists.txt already adds editor/webui BEFORE editor/shell, so the cache
# entry is populated by the time the Shell reads it; the Shell FATAL_ERRORs on an empty value rather
# than silently compiling in an empty asset root.
set(CONTEXT_WEBUI_ASSET_DIR "${_ctx_webui_assets}" CACHE INTERNAL
    "editor-core's built asset root, served by the Shell over context-editor://app/ (e05c)")

# --- tests (R-QA-013: behavior ships WITH its tests in the same PR) ------------------------------
# The `webui-*` family is a plain package test family: it is NOT in the build job's gate-exclusion
# regex, so it auto-runs in the general ctest step on all three OS legs with no ci.yml edit
# (CLAUDE.md § Test taxonomy & CI wiring rules).

# 1) The built asset set is real, Node-free, and every staged dockview file still matches its pin.
add_test(NAME webui-assets
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/../tools/check_webui_assets.py"
            --asset-dir "${_ctx_webui_assets}"
            --manifest "${_ctx_dockview_manifest}"
            --bundle-name "${_ctx_webui_bundle_name}")

# 1b) The e05c SCHEME CONTRACT gate. Three facts that live in two languages and would otherwise
# drift silently:
#   * the served document carries NO inline script and NO inline style — the CSP the Shell sends
#     would BLOCK them, so an edit that adds one produces a blank editor rather than a build error;
#   * the scheme vocabulary the TS bundle compiled in (scheme, origin, endpoint, query-function
#     name) is byte-identical to the C++ constants the Shell serves and routes with;
#   * no `file://` URL reached the asset set (a DoD line: "no file:// fallback exists").
# Checked against the BUILT asset, not the source, so it sees what the renderer will actually get.
add_test(NAME webui-scheme-contract
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/../tools/check_webui_assets.py"
            --asset-dir "${_ctx_webui_assets}"
            --manifest "${_ctx_dockview_manifest}"
            --bundle-name "${_ctx_webui_bundle_name}"
            --scheme-contract
            --shell-include-dir "${CMAKE_CURRENT_SOURCE_DIR}/../shell/include/context/editor/shell"
            --shell-cef-dir "${CMAKE_CURRENT_SOURCE_DIR}/../shell/cef/src")

# 1c) The e05d1 PANEL CONTRACT gate. The panel surface is the widest cross-language seam in the
# editor: six `panel.*` method names, two D6 state member names and four gesture verbs, each of which
# exists once in C++ and once in TS. A rename on either side unbinds the surface SILENTLY — the
# editor comes up with no panels and no build error anywhere. This re-reads all twelve out of the
# BUILT bundle and compares them to the C++ constants, and additionally asserts the two facts that
# would make the panel layer non-functional in a way no unit test can see: that the pinned docking
# engine is LOADED by the served document (rather than bundled, which would void `webui-assets`'
# SHA re-hash of the shipped artifact), and that editor-core's declared dependency set is still
# exactly the owner-ratified, version-pinned s1 set.
add_test(NAME webui-panel-contract
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/../tools/check_webui_assets.py"
            --asset-dir "${_ctx_webui_assets}"
            --manifest "${_ctx_dockview_manifest}"
            --bundle-name "${_ctx_webui_bundle_name}"
            --panel-contract
            --shell-include-dir "${CMAKE_CURRENT_SOURCE_DIR}/../shell/include/context/editor/shell"
            --contract-include-dir
                "${CMAKE_CURRENT_SOURCE_DIR}/../gui/contract/include/context/editor/gui/contract"
            --package-json "${_ctx_webui_core}/package.json"
            --shell-src-dir "${CMAKE_CURRENT_SOURCE_DIR}/../shell/src")

# 1d) The e14c WELCOME CONTRACT gate. The welcome surface adds four `welcome.*` method names plus the
# `welcome`/`project` mode tokens, each declared once in welcome.h (C++) and once in welcome.ts. A rename
# on either side unbinds the welcome screen or strands boot.ts's mode branch SILENTLY — the same class of
# no-build-error failure the panel/scheme gates catch. This re-reads the production-reachable constants
# out of the BUILT bundle and compares them to welcome.h.
add_test(NAME webui-welcome-contract
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/../tools/check_webui_assets.py"
            --asset-dir "${_ctx_webui_assets}"
            --manifest "${_ctx_dockview_manifest}"
            --bundle-name "${_ctx_webui_bundle_name}"
            --welcome-contract
            --shell-include-dir "${CMAKE_CURRENT_SOURCE_DIR}/../shell/include/context/editor/shell")

# 1e) The e06b THEME CONTRACT gate. `colors.panel` is a PER-THEME value and editor-core's first run
# follows the HOST's `prefers-color-scheme` (06 §4 / C-F22), so each live CEF smoke PINS the theme it
# means into its boot URL and hardcodes that theme's panel colour as its scan constant. Three facts,
# three files, no compiler between them: edit a built-in theme's panel colour, or re-point a smoke,
# and the scan looks for a colour the running editor never paints — the frame is correct, the editor
# is healthy, and the assertion fails anyway, with a full CI round-trip on a CEF-only leg as its only
# signal. (That is exactly how it failed: a CI host has no colour-scheme preference, so Chromium's
# `light` default booted `builtin.light` while the smokes scanned for Dark's #0a0a0a.) A SOURCE scan,
# so it rides the `webui-*` family on all three default `build` legs — and runs locally.
add_test(NAME webui-theme-contract
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/../tools/check_webui_assets.py"
            --asset-dir "${_ctx_webui_assets}"
            --manifest "${_ctx_dockview_manifest}"
            --bundle-name "${_ctx_webui_bundle_name}"
            --theme-contract
            --shell-cef-dir "${CMAKE_CURRENT_SOURCE_DIR}/../shell/cef/src"
            --themes-dir "${CMAKE_CURRENT_SOURCE_DIR}/tokens/themes")

# 2) The DRIFT gate (e05a DoD: "generated from e02's schema; drift-checked in CI"). Re-projects the
# BUILD-GENERATED schema — the live registry's output, not the committed copy — and refuses any byte
# difference against the committed typings. A registry change that does not reach the editor's
# typings therefore reds CI instead of shipping stale types. Mirrors e02's own committed-vs-generated
# schema drift gate (client-test_schema) one link further down the chain.
add_test(NAME webui-client-typings-drift
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/../tools/gen_client_typings.py"
            --schema "${CONTEXT_CLIENT_SCHEMA_GENERATED}"
            --out "${_ctx_webui_core}/src/generated/client-schema.ts"
            --check)

# 2b) The RAW-KEY-HANDLER lint (M9 e07d, design 05 §6): "no raw key handlers anywhere". A SOURCE scan
# (no browser), so it rides the `webui-*` family on all three default `build` legs — every keyboard
# capability must flow through the keymap + command registry, and a stray `keydown`/`onkeydown`
# handler that bypasses that path (an unmarked one) fails the build. `tools/check_no_raw_key_handlers.py`
# has its own pytest under tools/tests/ (the python-tests job). This is the T1 lint the DoD requires.
add_test(NAME webui-no-raw-key-handlers
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/../tools/check_no_raw_key_handlers.py"
            --sources "${_ctx_webui_core}/src")

# 2c) The TOKENS-ONLY lint (M9 e06c1, design 06 section 1: "components reference ONLY semantic
# tokens; raw values live in themes alone"). BLOCKING in T1 — a SOURCE scan, so it rides the `webui-*`
# family on all three default `build` legs and runs under a plain local `ctest --preset dev`; there is
# no browser and no build artifact in the loop.
#
# It rejects a raw colour ANYWHERE in a kit stylesheet, a raw value in a tokenised property family
# (fonts / shape / motion), any `var()` FALLBACK, and any non-`--ctx-` custom property. The fallback
# rule is what makes the others hold: `var(--ctx-colors-line, rgba(230, 237, 245, 0.25))` — the shape
# app.css shipped — is a raw literal wearing a token's clothes.
#
# PROVEN NON-VACUOUS, not merely asserted to be: `tools/tests/test_check_kit_tokens.py` plants a raw
# hex colour, a colour function, a named colour, a raw font size, a raw font weight, a raw font
# family, a raw border length, a raw duration/easing and a `var()` fallback into a temporary
# stylesheet and requires exit 1 on each — so the proof re-runs on every `python-tests` job rather
# than living in one PR description. `src/editor/webui/kit/README.md` records the jurisdiction it
# covers today and the one gap (box spacing) e06a publishes no token for.
add_test(NAME webui-kit-tokens-only
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/../tools/check_kit_tokens.py"
            --tokens-only
            --kit-styles "${_ctx_webui_kit}/styles")

# 2d) The CLOSED-ROLE-SET + ONE-STYLING-OWNER gate (M9 e06c1, design 04 section 4 step 4). A
# three-way agreement across three languages that no compiler can see:
#   * the C++ role vocabulary (`uitree::role_token`, gui/uitree/src/node.cpp),
#   * the kit's `WIDGET_CLASSES` map (kit/src/index.ts),
#   * the classes `kit/styles/*.css` actually style,
# in BOTH directions, plus the assertion that NO `.ctx-widget-` rule survives in app.css.
#
# The failure it prevents is silent by construction. `render_html` emits only the twelve tags
# `role_html_tag` maps to and the hydration runtime looks each node's role up in the map, so adding a
# THIRTEENTH role to the C++ enum ships real, interactive, completely unthemed DOM with no build
# error and nothing in any log. The role list is DERIVED from the three sources rather than restated
# here, so this gate cannot drift into agreeing with a copy of itself.
add_test(NAME webui-kit-role-coverage
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/../tools/check_kit_tokens.py"
            --role-coverage
            --kit-source "${_ctx_webui_kit}/src/index.ts"
            --kit-styles "${_ctx_webui_kit}/styles"
            --uitree-source "${CMAKE_CURRENT_SOURCE_DIR}/../gui/uitree/src/node.cpp"
            --app-stylesheet "${CMAKE_CURRENT_SOURCE_DIR}/app/app.css")

# --- 3) the editor-core T1 unit-test tier (M9 e07a; R-QA-013, design 09 §1 / 04) -----------------
# Until now the webui workspace had NO local TS unit-test tier: the PURE, DOM-free editor-core logic
# (the total parsers in panels.ts / editorstate.ts, the structural guards in bridge.ts / dockview.ts)
# was proven live ONLY by the CI-only `editor-cef-smoke-shell` leg (test.md § webui TS/DOM runtime
# blind spot; R-QA-013 flagged the gap). This tier closes it by bundling a SECOND esbuild entry — the
# test bundle — through the SAME pinned esbuild + the SAME typecheck stamp (NO new npm dependency; a
# tiny in-repo assert shim under core/src/test/), then running it in the headless Chromium the CI
# already provisions.
#
# It is a CI-ONLY BROWSER PATH, exactly like editor-cef-smoke / render-web: default OFF, so the
# browser-less local Windows `dev` gate and the 3-OS `build` matrix never register a test they cannot
# run. The dedicated `webui-tests` CI job (ubuntu, headless google-chrome) configures
# -DCONTEXT_WEBUI_BROWSER_TESTS=ON, BUILDS `context_editor_webui_test` (its --target list) AND runs
# `ctest -R "^webui-ts-"` (its named step) — BOTH halves are load-bearing: miss either and the ctest
# reports "Not Run" -> RED (CE #264, the "Not Run = RED" tripwire). The test SOURCES under
# core/src/test/ still ride the production tsgo glob above, so they are TYPECHECKED on every one of
# the 3 `build` OS legs regardless of this option; only the browser-executed assertions run on ubuntu
# (the parse logic is a language property, not a platform one — the render / editor-boundary
# single-OS-blocking precedent).
option(CONTEXT_WEBUI_BROWSER_TESTS
    "Build + register the headless-Chromium editor-core TS unit tier (webui-ts-*); CI-only browser path (default OFF)."
    OFF)

if(CONTEXT_WEBUI_BROWSER_TESTS)
    set(_ctx_webui_test_entry "${_ctx_webui_core}/src/test/main.ts")
    set(_ctx_webui_test_assets "${CMAKE_BINARY_DIR}/editor-webui/test")
    set(_ctx_webui_test_bundle_name "editor-core.test.js")
    set(_ctx_webui_test_bundle "${_ctx_webui_test_assets}/${_ctx_webui_test_bundle_name}")

    # Bundle the test entry through the SAME pinned esbuild and gate it on the SAME typecheck stamp as
    # the production bundle — a type error in a test is a BUILD failure, not merely a test failure
    # (the e05a DoD, extended to the tests). Same flags as the production bundle: a browser ESM module
    # the headless Chromium loads directly.
    add_custom_command(
        OUTPUT "${_ctx_webui_test_bundle}"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${_ctx_webui_test_assets}"
        COMMAND "${CONTEXT_ESBUILD_BIN}" "${_ctx_webui_test_entry}"
                --bundle
                --format=esm
                --platform=browser
                --target=es2022
                "--outfile=${_ctx_webui_test_bundle}"
        DEPENDS ${_ctx_webui_inputs} "${_ctx_webui_stamps}/typecheck.stamp"
        COMMENT "context_editor_webui_test: bundling the editor-core T1 test entry (pinned esbuild)"
        VERBATIM)

    # Stage the test-only harness page beside the test bundle (served by webui_test_run.py, NOT the
    # Shell — no CSP applies). copy_if_different keeps incremental builds quiet.
    add_custom_command(
        OUTPUT "${_ctx_webui_test_assets}/harness.html"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${_ctx_webui_test_assets}"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                "${CMAKE_CURRENT_SOURCE_DIR}/test/harness.html"
                "${_ctx_webui_test_assets}/harness.html"
        DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/test/harness.html"
        COMMENT "context_editor_webui_test: staging the T1 harness page"
        VERBATIM)

    # Stage the REAL stylesheets + the REAL pinned docking engine beside the test bundle.
    #
    # LOAD-BEARING, not convenience. `core/src/test/theme_dom.test.ts` asserts what the browser
    # actually COMPUTES for Dockview's `--dv-*` chrome variables, and that value is decided by the
    # cascade between app.css, the vendored `dockview.css` and the copy dockview-core injects at
    # RUNTIME. Serving the harness without these three makes that whole file vacuous — so it is the
    # SHIPPED bytes that are staged (the same files the app asset root gets), never a fixture: a
    # fixture would pass while the shipped stylesheet failed, which is precisely the defect the tier
    # exists to catch one CI round-trip earlier than `editor-cef-smoke-shell` does.
    set(_ctx_webui_test_web_assets "")
    foreach(_member IN LISTS _ctx_dockview_members)
        add_custom_command(
            OUTPUT "${_ctx_webui_test_assets}/${_member}"
            COMMAND "${CMAKE_COMMAND}" -E make_directory "${_ctx_webui_test_assets}"
            COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                    "${_ctx_dockview_dest}/${_member}" "${_ctx_webui_test_assets}/${_member}"
            DEPENDS "${_ctx_dockview_dest}/${_member}"
            COMMENT "context_editor_webui_test: staging ${_member} for the DOM tier"
            VERBATIM)
        list(APPEND _ctx_webui_test_web_assets "${_ctx_webui_test_assets}/${_member}")
    endforeach()
    add_custom_command(
        OUTPUT "${_ctx_webui_test_assets}/app.css"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${_ctx_webui_test_assets}"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                "${CMAKE_CURRENT_SOURCE_DIR}/app/app.css" "${_ctx_webui_test_assets}/app.css"
        DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/app/app.css"
        COMMENT "context_editor_webui_test: staging app.css for the DOM tier"
        VERBATIM)
    list(APPEND _ctx_webui_test_web_assets "${_ctx_webui_test_assets}/app.css")

    # The SHIPPED kit stylesheet, for the same load-bearing reason as app.css above: `kit.test.ts`
    # asserts what the browser COMPUTED for each of the twelve widget classes and looks the sheet up
    # by name, so a harness served without it fails loudly instead of passing vacuously. A fixture
    # copy would be worse than nothing — it would pass while the shipped stylesheet was broken.
    add_custom_command(
        OUTPUT "${_ctx_webui_test_assets}/${_ctx_webui_kit_stylesheet_name}"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${_ctx_webui_test_assets}"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                "${_ctx_webui_kit}/styles/${_ctx_webui_kit_stylesheet_name}"
                "${_ctx_webui_test_assets}/${_ctx_webui_kit_stylesheet_name}"
        DEPENDS "${_ctx_webui_kit}/styles/${_ctx_webui_kit_stylesheet_name}"
        COMMENT "context_editor_webui_test: staging kit.css for the DOM tier"
        VERBATIM)
    list(APPEND _ctx_webui_test_web_assets
         "${_ctx_webui_test_assets}/${_ctx_webui_kit_stylesheet_name}")

    # NOT an ALL target: built only on demand by the webui-tests CI job's --target list (and a local
    # -DCONTEXT_WEBUI_BROWSER_TESTS=ON repro). Keeping it out of ALL is what lets the default 3-OS
    # build stay browser-free while the test SOURCES still typecheck there via the production glob.
    add_custom_target(context_editor_webui_test
        DEPENDS "${_ctx_webui_test_bundle}" "${_ctx_webui_test_assets}/harness.html"
                ${_ctx_webui_test_web_assets})

    # The headless-Chromium unit run. webui_test_run.py serves the test asset dir, runs the bundle in
    # a Chromium-family browser, and exits non-zero on any failed assertion (or a missing browser /
    # missing bundle).
    add_test(NAME webui-ts-unit
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/../tools/webui_test_run.py"
                --asset-dir "${_ctx_webui_test_assets}"
                --page harness.html
                --bundle-name "${_ctx_webui_test_bundle_name}")
endif()
