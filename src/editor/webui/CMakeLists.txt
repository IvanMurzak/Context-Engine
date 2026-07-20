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
set(_ctx_webui_inputs ${_ctx_webui_ts} "${_ctx_webui_tsconfig}" "${_ctx_webui_core}/package.json")

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
            --package-json "${_ctx_webui_core}/package.json")

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
