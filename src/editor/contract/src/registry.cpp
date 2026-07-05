// Registry implementation: the single source of truth + its surface projections (see registry.h).

#include "context/editor/contract/registry.h"

#include "context/editor/contract/error_catalog.h"
#include "context/editor/contract/handshake.h"
#include "context/editor/contract/resource_handle.h"
#include "context/editor/schema/kind_schema.h"

#include <utility>

namespace context::editor::contract
{

std::string VerbSpec::cli_command() const
{
    std::string cmd = "context ";
    if (!noun.empty())
    {
        if (!ns.empty())
            cmd += ns + ":";
        cmd += noun + " ";
    }
    cmd += verb;
    return cmd;
}

std::string VerbSpec::key() const
{
    return (ns.empty() ? std::string() : ns + ":") + noun + "/" + verb;
}

namespace
{
// The R-CLI-007 core-flag set: honored by EVERY verb. --idempotency-key, --after-generation and
// --atomic-plan stay reserved-but-accepted in v1 (their behavior activates with the replay store /
// the batch backend); --after-hash is LIVE — the own-write barrier mechanism it names
// (EditorKernel::query_after_hash) exists and is tested.
std::vector<FlagSpec> make_core_flags()
{
    return {
        {"json", "bool", "Emit the R-CLI-008 result envelope as JSON on stdout.", false},
        {"project", "path", "Path to the project root the verb operates against.", false},
        {"if-match", "hash",
         "CAS guard: apply only if the target's raw-byte hash matches (R-CLI-006).", false},
        {"after-generation", "generation",
         "Read-your-writes barrier: block until the derived world reflects this generation "
         "(R-CLI-006).",
         true},
        {"dry-run", "bool", "Validate and report the intended effect without writing.", false},
        {"idempotency-key", "string",
         "Client-supplied replay key (R-CLI-016); reserved and accepted, replay store lands later.",
         true},
        {"after-hash", "hash",
         "Own-write read barrier: block until the target path's derived node reflects the "
         "canonical hash the write returned (R-CLI-006).",
         false},
        {"atomic-plan", "bool",
         "Plan-level all-or-nothing batch: CAS every target against its planning-time hash and "
         "refuse the whole batch if any target moved (R-CLI-011); reserved — the batch backend "
         "activates the behavior later.",
         true},
    };
}

// Derive the stable RPC method-id from the grammar triple: "<noun>.<verb>" ("<verb>" when global).
std::string rpc_method_for(const std::string& noun, const std::string& verb)
{
    return noun.empty() ? verb : noun + "." + verb;
}

// Derive the MCP tool name: "context_[<noun>_]<verb>", folding '-' to '_' so a hyphenated verb
// (e.g. "edit-batch") yields a conventional snake_case tool name ("context_edit_batch").
std::string mcp_tool_for(const std::string& noun, const std::string& verb)
{
    std::string tool = noun.empty() ? "context_" + verb : "context_" + noun + "_" + verb;
    for (char& ch : tool)
    {
        if (ch == '-')
            ch = '_';
    }
    return tool;
}

VerbSpec make_verb(std::string ns, std::string noun, std::string verb, std::string summary,
                   std::vector<ParamSpec> params, std::vector<FlagSpec> flags, bool implemented,
                   std::string stability = "stable", std::string cli_alias = std::string())
{
    VerbSpec spec;
    spec.rpc_method = rpc_method_for(noun, verb);
    spec.mcp_tool = mcp_tool_for(noun, verb);
    spec.ns = std::move(ns);
    spec.noun = std::move(noun);
    spec.verb = std::move(verb);
    spec.summary = std::move(summary);
    spec.params = std::move(params);
    spec.flags = std::move(flags);
    spec.implemented = implemented;
    spec.stability = std::move(stability);
    spec.cli_alias = std::move(cli_alias);
    return spec;
}

Json param_to_json(const ParamSpec& p)
{
    Json out = Json::object();
    out.set("name", Json(p.name));
    out.set("type", Json(p.value_type));
    out.set("required", Json(p.required));
    out.set("description", Json(p.description));
    return out;
}

Json flag_to_json(const FlagSpec& f)
{
    Json out = Json::object();
    out.set("name", Json(f.name));
    out.set("type", Json(f.value_type));
    out.set("description", Json(f.description));
    out.set("reserved", Json(f.reserved));
    return out;
}

// The flat name list of every input a verb accepts on any surface: params + core flags + verb
// flags. Shared by all three surface generators so parity is exact.
Json param_names(const VerbSpec& v)
{
    Json names = Json::array();
    for (const ParamSpec& p : v.params)
        names.push_back(Json(p.name));
    return names;
}

Json flag_names(const VerbSpec& v, const std::vector<FlagSpec>& core)
{
    Json names = Json::array();
    for (const FlagSpec& f : core)
        names.push_back(Json(f.name));
    for (const FlagSpec& f : v.flags)
        names.push_back(Json(f.name));
    return names;
}
} // namespace

Registry::Registry()
{
    core_flags_ = make_core_flags();

    // The M1 verb surface. Kept small — enough to fix the grammar (global + noun-scoped
    // + reserved-package forms), the envelope, describe, and a runnable-template scaffolder — while
    // the query/batch/subscription verbs (R-CLI-011/012/014/015) reserve their grammar for M3.
    verbs_.push_back(make_verb(
        "", "", "describe",
        "Emit the whole self-describing contract (verbs, methods, tools, topics, error catalog, "
        "protocol) as JSON.",
        /*params=*/{}, /*flags=*/{}, /*implemented=*/true));

    verbs_.push_back(make_verb(
        "", "", "new",
        "Scaffold a new project from a template. The default template is a minimal RUNNABLE "
        "skeleton (a scene, a camera, a startable session) — R-QA-006.",
        /*params=*/
        {{"directory", "path", true, "Target directory to scaffold the project into."},
         {"template", "string", false,
          "Template name; defaults to the runnable default template."}},
        /*flags=*/{}, /*implemented=*/true));

    // The composed WRITE path (M2, R-CLI-006 / L-35): `context set` writes a value onto a composed
    // entity — an override entry in the OUTERMOST (root) instancing scene by default, the defining
    // template with `--edit-template`, or a mid-level instancing scene with `--at-instance`. The
    // write is a canonical file rewrite through filesync's R-FILE-004 atomic write path (the core
    // `--if-match` flag is the raw-byte CAS guard); the envelope reports the file + JSON-pointer
    // actually written and both labelled resulting hashes. Addressing uses the L-35 id-path form.
    verbs_.push_back(make_verb(
        "", "", "set",
        "Write a value onto a composed entity (R-CLI-006 / L-35): an override entry in the "
        "outermost instancing scene by default, the defining template (--edit-template), or a "
        "mid-level instancing scene (--at-instance). A canonical file rewrite through the R-FILE-004 "
        "atomic write path; the envelope reports the file + JSON-pointer written + both labelled "
        "resulting hashes.",
        /*params=*/
        {{"path", "path", true, "The root scene file to compose + write against (project-relative)."},
         {"value", "json", true, "The JSON value to set at the addressed field."}},
        /*flags=*/
        {{"pointer", "string", "RFC 6901 JSON pointer of the field to write inside the entity.",
          false},
         {"id-path", "string",
          "The L-35 id-path to the composed entity, slash-separated ([instanceId, ..., entityId]).",
          false},
         {"edit-template", "bool",
          "Retarget the write to the entity's DEFINING template file (edit it in place) instead of "
          "writing an override in the outermost scene.",
          false},
         {"at-instance", "string",
          "Retarget the override to the mid-level instancing scene named by this id-path prefix "
          "(a strict prefix of --id-path).",
          false}},
        /*implemented=*/true));

    // The L-37 explicit bulk migration path (M2 wave 3, R-DATA-004): rewrite otherwise-untouched
    // files to current schema versions — the ONLY migration that writes disk besides tool saves
    // (parse-time migration is in-memory). Canonicalizes + stamps every file it rewrites; files
    // with blocking findings (newer-than stamps, chain gaps, failed steps) are reported and left
    // untouched. Honors the core --dry-run and --project flags.
    verbs_.push_back(make_verb(
        "", "", "migrate",
        "Bulk-migrate authored JSON files to the current registered schema versions (L-37): "
        "canonicalize + migrate stamped-older component payloads + stamp current versions, "
        "rewriting files in place. The explicit disk-writing migration path; parse-time "
        "migration never touches disk.",
        /*params=*/
        {{"path", "path", false,
          "File or directory to migrate (recursive over *.json); defaults to the --project "
          "root."}},
        /*flags=*/{}, /*implemented=*/true));

    // A noun-scoped, package-facing verb: fixes `context <noun> <verb>` grammar AND reserves the
    // R-CLI-007 namespace-collision path (inert until a second package source exists).
    verbs_.push_back(make_verb(
        "", "package", "add",
        "Add a package to the project. Its reserved namespace is collision-checked at add time "
        "(R-CLI-007); the check activates when a second package source exists.",
        /*params=*/{{"name", "string", true, "Package name/source to add."}},
        /*flags=*/{}, /*implemented=*/false));

    // The R-CLI-017 large-result fetch verb. The opaque-URI handle FORMAT and this
    // `resource.read { handle, range }` verb are contract from day one (the wire shape cannot be
    // retrofitted); v1 implements same-filesystem fetch only — the URI resolves against the local
    // daemon (bridge::ResourceStore). `context fetch` is the registry-owned CLI alias R-CLI-017
    // names for it.
    verbs_.push_back(make_verb(
        "", "resource", "read",
        "Read (a byte range of) an oversized result payload by its transport-portable opaque "
        "resource handle (R-CLI-017). Served by a live daemon; v1 resolves same-filesystem only.",
        /*params=*/
        {{"handle", "string", true,
          "The opaque resource URI (context-res://...) a largeResult envelope returned."},
         {"range", "json", false,
          "Optional byte range {offsetBytes, lengthBytes}; omitted reads from 0 up to the "
          "daemon's chunk cap."}},
        /*flags=*/
        {{"out", "path",
          "Also write the result envelope to this file (the operational sink `context attach` / "
          "`context daemon` offer).",
          false}},
        /*implemented=*/true, /*stability=*/"stable", /*cli_alias=*/"fetch"));

    // M2 wave 3 (issue #53): the asset-database verbs (L-36 / R-ASSET-002). The engine operation
    // (meta-first dependency-safe order, idempotent under partial apply — R-FILE-004) ships in
    // src/editor/assetdb/; the grammar is contract now and the CLI/daemon serving wire lands with
    // the M2 integration task (the `set` pattern). Referencing files are never rewritten by these
    // verbs — path hints heal lazily on tool save (L-34).
    verbs_.push_back(make_verb(
        "", "asset", "move",
        "Move an asset (its <asset>.meta.json sidecar travels with it) to a new path, preserving "
        "its GUID identity (L-36). Never overwrites an occupied destination.",
        /*params=*/
        {{"from", "path", true, "Project-relative source asset path."},
         {"to", "path", true, "Project-relative destination asset path."}},
        /*flags=*/{}, /*implemented=*/false));

    verbs_.push_back(make_verb(
        "", "asset", "rename",
        "Rename an asset in place, keeping its <asset>.meta.json sidecar and GUID in sync (L-36) "
        "— the same meta-first engine operation as `asset move` (R-FILE-004).",
        /*params=*/
        {{"from", "path", true, "Project-relative asset path."},
         {"to", "path", true, "The new project-relative asset path."}},
        /*flags=*/{}, /*implemented=*/false));

    // M2 wave 4 (issue #59): the structural three-way merge family (R-FILE-012 / L-26/L-33/L-37) —
    // the convergence primitive for worktree-per-agent parallelism. Registered here at the END of the
    // stable verbs block (this anchor, not the operational block below) so concurrent M2 siblings
    // inserting verbs merge cleanly. Backed by src/editor/merge/ + src/cli/merge_command.cpp.
    verbs_.push_back(make_verb(
        "", "", "merge-file",
        "Schema-aware structural three-way merge over authored JSON (R-FILE-012): field-path "
        "granular, id-based merge identity (L-33), machine-readable conflict envelope — never "
        "last-writer-wins, never text conflict markers. `--driver` is git's merge-driver entry.",
        /*params=*/
        {{"base", "path", true, "The common-ancestor file (git's %O)."},
         {"ours", "path", true, "The current-branch file (git's %A; also the default output)."},
         {"theirs", "path", true, "The incoming-branch file (git's %B)."},
         {"pathname", "path", false,
          "The real repository path (git's %P); names the conflict sidecar. Defaults to --output."}},
        /*flags=*/
        {{"output", "path", "Where to write the merged result; defaults to the ours/%A path.", false},
         {"driver", "bool",
          "git merge-driver mode: write the result to the ours/%A path and exit non-zero on "
          "unresolved conflicts (L-27: git invokes the driver, never the reverse).",
          false}},
        /*implemented=*/true));

    verbs_.push_back(make_verb(
        "", "", "resolve-conflict",
        "Apply one resolution to a merged file per conflict entry (R-FILE-012): take a whole side "
        "or write an explicit value, looping until the conflict sidecar empties.",
        /*params=*/{{"file", "path", true, "The merged file to resolve in place."}},
        /*flags=*/
        {{"path", "string", "The RFC 6901 field-path of the conflict entry to resolve.", false},
         {"take", "string", "Take one whole side of the conflict: 'ours' or 'theirs'.", false},
         {"value", "json", "Write an explicit JSON value at --path (instead of --take).", false}},
        /*implemented=*/true));

    verbs_.push_back(make_verb(
        "", "", "re-key",
        "Mint a fresh stable id for a duplicated intra-file entity and rewrite its unambiguous "
        "in-file references (R-FILE-012(c) convergence remedy for a duplicate-id).",
        /*params=*/{{"file", "path", true, "The file to re-key in place."}},
        /*flags=*/
        {{"at", "string", "RFC 6901 pointer to the object to re-key.", false},
         {"id", "string", "Re-key the last holder of this duplicated intra-file id.", false}},
        /*implemented=*/true));

    verbs_.push_back(make_verb(
        "", "", "validate",
        "The post-merge convergence gate (R-FILE-012): report the duplicate-intra-file-id "
        "diagnostic (merge.duplicate_id) across a file or the project tree.",
        /*params=*/
        {{"path", "path", false,
          "File or directory to validate (recursive over *.json); defaults to the --project root."}},
        /*flags=*/{}, /*implemented=*/true));

    // --- M3 entry: headless session control + versioned replay (issue #74, R-QA-005 / L-54) ------
    // The deterministic headless harness surface (R-CLI-009 the ONE registry): session control
    // (new / step / seed / inject / hash / record) over a session-state file, plus the global
    // `replay` verb. New verbs enter the ONE registry so the CLI ≡ RPC ≡ MCP ≡ introspection parity
    // + additive-catalog gates stay green (protocolMajor stays 0 — additive-only). The determinism
    // auto-triage grammar (`determinism diff`) is RESERVED here (implemented=false): the grammar is
    // contract from day one, its bisect/triage backing lands in the follow-up split (the issue's
    // HALT-to-split candidate). Inserted at THIS anchor (end of the stable block, before the
    // operational surface) so a future sibling insertion merges cleanly.
    verbs_.push_back(make_verb(
        "", "session", "new",
        "Create a headless simulation session-state file (R-QA-005): a fresh deterministic session "
        "for the given --seed + --scenario. The state file persists the session between the one-shot "
        "session verbs (step / seed / inject / hash / record).",
        /*params=*/{{"state", "path", true, "Path to the session-state file to create."}},
        /*flags=*/
        {{"seed", "string", "The deterministic PRNG seed (unsigned integer); defaults to 0.", false},
         {"scenario", "string", "The headless scenario to run; defaults to the built-in `demo`.",
          false}},
        /*implemented=*/true));

    verbs_.push_back(make_verb(
        "", "session", "step",
        "Advance a session exactly N fixed ticks (--ticks; R-SIM-002 fixed timestep). The result "
        "envelope carries the monotonic simTick (R-CLI-016 — a lost ack reads the counter and steps "
        "only the missing delta) and the resulting hierarchical state hash.",
        /*params=*/{{"state", "path", true, "The session-state file to advance in place."}},
        /*flags=*/
        {{"ticks", "string", "The number of fixed ticks to advance (unsigned integer); defaults to 1.",
          false},
         {"trace", "bool", "Record the per-tick hierarchical hash tree (trace mode).", false}},
        /*implemented=*/true));

    verbs_.push_back(make_verb(
        "", "session", "seed",
        "Query the session seed, or set it with --set (rebuilding the seeded initial world; valid "
        "before stepping). R-QA-005 seed set/query.",
        /*params=*/{{"state", "path", true, "The session-state file."}},
        /*flags=*/
        {{"set", "string",
          "Set the seed to this unsigned integer and rebuild the initial world.", false}},
        /*implemented=*/true));

    verbs_.push_back(make_verb(
        "", "session", "inject",
        "Inject a synthetic input event (--event <device> --code <code>) or a mapped action "
        "activation (--action <name>) into the session at --at (default: the current simTick). "
        "Gameplay + UI actions share the started/performed/canceled phases (R-QA-005 injection).",
        /*params=*/{{"state", "path", true, "The session-state file to inject into."}},
        /*flags=*/
        {{"action", "string",
          "Inject a mapped action activation with this name (e.g. move_x, ui_submit).", false},
         {"event", "string", "Inject a raw input event with this device (paired with --code).",
          false},
         {"code", "string", "The code on the --event device (e.g. W).", false},
         {"phase", "string", "The action phase: started | performed | canceled (default performed).",
          false},
         {"value", "string", "The integer value of the event/action (default 1).", false},
         {"at", "string", "The sim tick to schedule the injection at (default: the current simTick).",
          false}},
        /*implemented=*/true));

    verbs_.push_back(make_verb(
        "", "session", "hash",
        "Query the session's canonical hierarchical state hash (R-QA-005): the root plus the "
        "per-archetype sub-hashes of the current persisted state. (The per-tick hash TRACE is a "
        "property of a run, not a persisted snapshot — obtain it from `session step --trace`.)",
        /*params=*/{{"state", "path", true, "The session-state file."}},
        /*flags=*/{}, /*implemented=*/true));

    verbs_.push_back(make_verb(
        "", "session", "record",
        "Record a versioned replay artifact from the session's seed + recorded input stream (the "
        "ctx:replay kind, R-QA-005): input stream + seed + tick count + engine/protocol versions + "
        "an optional content-hash manifest of project inputs + the expected per-tick hash trace.",
        /*params=*/{{"state", "path", true, "The session-state file to record from."}},
        /*flags=*/
        {{"out", "path", "Write the replay artifact to this path (default: inline in the envelope).",
          false},
         {"manifest", "string",
          "Comma-separated project-relative input paths to content-hash into the manifest.", false},
         {"non-deterministic", "bool", "Mark the artifact best-effort (no expected trace recorded).",
          false}},
        /*implemented=*/true));

    verbs_.push_back(make_verb(
        "", "", "replay",
        "Replay a versioned replay artifact (R-QA-005): verify its content manifest against the "
        "project BEFORE running (drift is reported, never a silent divergence), re-run the input "
        "stream, and report the first-divergence tick. A non-deterministic artifact is best-effort.",
        /*params=*/{{"artifact", "path", true, "The replay artifact file to run."}},
        /*flags=*/{}, /*implemented=*/true));

    verbs_.push_back(make_verb(
        "", "determinism", "diff",
        "Triage a determinism divergence between two runs/artifacts (R-QA-005): replay-bisect to the "
        "first divergent tick, diff the first divergent system, and report (tick, system, entity, "
        "componentField). RESERVED — the hierarchical-hash trace it consumes ships now; the "
        "auto-triage backing lands in the follow-up split.",
        /*params=*/
        {{"left", "path", true, "The baseline run/artifact."},
         {"right", "path", true, "The divergent run/artifact to triage against the baseline."}},
        /*flags=*/{}, /*implemented=*/false));

    // --- the OPERATIONAL daemon-driver surface (R-CLI-009 honesty) ------------------------------
    // These RPC methods are genuinely served by a live daemon's method backend (KernelServer) — the
    // cross-process analogue of `context editor smoke`. They are registered here so
    // `context describe` reflects the REAL served surface, but marked stability="operational":
    // explicitly UNSTABLE, promoted into the stable contract (or dropped) at the M3 freeze. They are
    // NOT one-shot CLI verbs (the CLI rejects them with contract.operational_only).
    verbs_.push_back(make_verb(
        "", "", "edit",
        "Daemon-initiated single-file write through filesync atomic-IO with the read-your-writes "
        "barrier (R-FILE-004 / R-CLI-006). Requires the file_write scope.",
        /*params=*/
        {{"path", "path", true, "Project-relative file to write."},
         {"content", "string", true, "The full content to write (tool saves canonicalize JSON)."}},
        /*flags=*/{}, /*implemented=*/true, /*stability=*/"operational"));

    verbs_.push_back(make_verb(
        "", "", "edit-batch",
        "Daemon-initiated MULTI-file write serialized through the R-FILE-004 crash-recovery intent "
        "log. Requires the file_write scope.",
        /*params=*/
        {{"files", "json", true, "Non-empty array of {path, content} objects."}},
        /*flags=*/{}, /*implemented=*/true, /*stability=*/"operational"));

    verbs_.push_back(make_verb(
        "", "", "query",
        "Read the derived node for a path (canonical hash + generation) plus world stats. NOT the "
        "R-CLI-012 query language — a single-path operational read. `--overrides <mode> <scene>` is "
        "the one-shot advisory override-hygiene read (R-CLI-006), served over the project scenes.",
        /*params=*/{{"path", "string", true, "Project-relative path to look up."}},
        /*flags=*/
        {{"overrides", "string",
          "Advisory override-hygiene read (R-CLI-006, never auto-pruned): `diverged` lists "
          "overrides whose recorded base no longer matches the current template value; `redundant` "
          "lists overrides equal to the template value. Served one-shot over the project's scenes "
          "(the plain `query` is daemon-served).",
          false}},
        /*implemented=*/true, /*stability=*/"operational"));

    verbs_.push_back(make_verb(
        "", "", "snapshot",
        "The R-BRIDGE-008 current-state snapshot (incarnationId, generation, lastSeq, world stats) "
        "plus the boot-time R-FILE-004 recovery diagnostics.",
        /*params=*/{}, /*flags=*/{}, /*implemented=*/true, /*stability=*/"operational"));

    verbs_.push_back(make_verb(
        "", "", "reconcile",
        "Fold external (out-of-band) edits into the derived world: drain watcher hints and force "
        "the full re-hash crawl (R-FILE-002), then settle.",
        /*params=*/{}, /*flags=*/{}, /*implemented=*/true, /*stability=*/"operational"));

    verbs_.push_back(make_verb(
        "", "", "build",
        "Trigger a build. Scope-mapped (build_install, R-SEC-007) and registered for honesty; the "
        "backing is not served yet.",
        /*params=*/{}, /*flags=*/{}, /*implemented=*/false, /*stability=*/"operational"));

    verbs_.push_back(make_verb(
        "", "", "shutdown",
        "Ask the daemon's serve loop to stop after replying. Requires the session_control scope.",
        /*params=*/{}, /*flags=*/{}, /*implemented=*/true, /*stability=*/"operational"));

    // The R-BRIDGE-008 core event topics, described statically (R-CLI-013/014). Live
    // package-registered topics join this set at runtime as the package ecosystem lands.
    topics_ = {
        {"files", "Post-derivation file change facts (never raw filesystem noise)."},
        {"derivation", "Derived-world updates, each stamped with the input content-hash."},
        {"diagnostics", "Machine-readable diagnostics through the R-CLI-008 error schema."},
        {"session", "Session lifecycle + restart-class reload announcements."},
        {"clients", "Client attach/detach on the daemon."},
        {"log", "Structured log entries (severity, source, tick, session)."},
    };

    // The engine file kinds (R-CLI-005 / R-DATA-006, M2 wave 2): each registered kind's versioned
    // schema publication, projected from the SAME schema module the derivation validate node
    // enforces — the registry stays the one enumeration surface without becoming a second source
    // of truth. Package-contributed kinds join through the same register_file_kind() mechanism.
    for (const schema::KindSchema& kind : schema::engine_schemas().all())
        register_file_kind(
            {kind.id, kind.version, Json::parse(schema::introspection_json(kind))});
}

void Registry::register_file_kind(FileKindSpec spec)
{
    for (FileKindSpec& existing : file_kinds_)
        if (existing.id == spec.id && existing.version == spec.version)
        {
            existing = std::move(spec);
            return;
        }
    file_kinds_.push_back(std::move(spec));
}

const FileKindSpec* Registry::find_file_kind(const std::string& id) const
{
    for (const FileKindSpec& kind : file_kinds_)
        if (kind.id == id)
            return &kind;
    return nullptr;
}

const Registry& Registry::instance()
{
    static const Registry registry;
    return registry;
}

const VerbSpec* Registry::find_verb(const std::string& ns, const std::string& noun,
                                    const std::string& verb) const
{
    for (const VerbSpec& v : verbs_)
        if (v.ns == ns && v.noun == noun && v.verb == verb)
            return &v;
    return nullptr;
}

Json Registry::cli_surface() const
{
    Json out = Json::array();
    for (const VerbSpec& v : verbs_)
    {
        Json entry = Json::object();
        entry.set("command", Json(v.cli_command()));
        if (!v.cli_alias.empty())
            entry.set("alias", Json("context " + v.cli_alias));
        entry.set("ns", Json(v.ns));
        entry.set("noun", Json(v.noun));
        entry.set("verb", Json(v.verb));
        entry.set("stability", Json(v.stability));
        entry.set("params", param_names(v));
        entry.set("flags", flag_names(v, core_flags_));
        out.push_back(std::move(entry));
    }
    return out;
}

Json Registry::rpc_surface() const
{
    Json out = Json::array();
    for (const VerbSpec& v : verbs_)
    {
        Json entry = Json::object();
        entry.set("method", Json(v.rpc_method));
        entry.set("ns", Json(v.ns));
        entry.set("noun", Json(v.noun));
        entry.set("verb", Json(v.verb));
        entry.set("stability", Json(v.stability));
        entry.set("params", param_names(v));
        entry.set("flags", flag_names(v, core_flags_));
        out.push_back(std::move(entry));
    }
    return out;
}

Json Registry::mcp_surface() const
{
    Json out = Json::array();
    for (const VerbSpec& v : verbs_)
    {
        // The MCP tool's input schema exposes params + flags as named inputs; parity with the CLI
        // and RPC surfaces is checked on these same name lists.
        Json input_schema = Json::object();
        input_schema.set("params", param_names(v));
        input_schema.set("flags", flag_names(v, core_flags_));

        Json entry = Json::object();
        entry.set("tool", Json(v.mcp_tool));
        entry.set("ns", Json(v.ns));
        entry.set("noun", Json(v.noun));
        entry.set("verb", Json(v.verb));
        entry.set("stability", Json(v.stability));
        entry.set("params", param_names(v));
        entry.set("flags", flag_names(v, core_flags_));
        entry.set("inputSchema", std::move(input_schema));
        out.push_back(std::move(entry));
    }
    return out;
}

Json Registry::describe() const
{
    Json contract = Json::object();

    contract.set("protocol", protocol_descriptor());

    Json core = Json::array();
    for (const FlagSpec& f : core_flags_)
        core.push_back(flag_to_json(f));
    contract.set("coreFlags", std::move(core));

    Json verbs = Json::array();
    for (const VerbSpec& v : verbs_)
    {
        Json entry = Json::object();
        entry.set("ns", Json(v.ns));
        entry.set("noun", Json(v.noun));
        entry.set("verb", Json(v.verb));
        entry.set("command", Json(v.cli_command()));
        if (!v.cli_alias.empty())
            entry.set("cliAlias", Json("context " + v.cli_alias));
        entry.set("summary", Json(v.summary));
        entry.set("rpcMethod", Json(v.rpc_method));
        entry.set("mcpTool", Json(v.mcp_tool));
        entry.set("implemented", Json(v.implemented));
        entry.set("stability", Json(v.stability));
        Json params = Json::array();
        for (const ParamSpec& p : v.params)
            params.push_back(param_to_json(p));
        entry.set("params", std::move(params));
        Json flags = Json::array();
        for (const FlagSpec& f : v.flags)
            flags.push_back(flag_to_json(f));
        entry.set("flags", std::move(flags));
        verbs.push_back(std::move(entry));
    }
    contract.set("verbs", std::move(verbs));

    contract.set("rpcMethods", rpc_surface());
    contract.set("mcpTools", mcp_surface());

    Json topics = Json::array();
    for (const TopicSpec& t : topics_)
    {
        Json entry = Json::object();
        entry.set("name", Json(t.name));
        entry.set("description", Json(t.description));
        topics.push_back(std::move(entry));
    }
    contract.set("eventTopics", std::move(topics));

    Json errors = Json::array();
    for (const ErrorCode& e : catalog())
    {
        Json entry = Json::object();
        entry.set("code", Json(e.code));
        entry.set("message", Json(e.message));
        entry.set("retriable", Json(e.retriable));
        entry.set("exitCode", Json(e.exit_code));
        entry.set("origin", Json(e.origin));
        errors.push_back(std::move(entry));
    }
    contract.set("errorCatalog", std::move(errors));

    // The R-CLI-017 large-result mechanism: URI scheme, fetch verb naming, chunk encoding, and the
    // handle / read-result field shapes (the numeric spool threshold is daemon policy, not shape).
    contract.set("largeResult", large_result_descriptor());

    // The registered file kinds (R-CLI-005 / R-DATA-006): one entry per kind — id, schema
    // version, the derived per-field x-ctx-* index (units/storage/ref/union metadata), and the
    // full published JSON Schema. Enumerated LIVE from the registration set, so `describe` and
    // the derivation validate node can never disagree on what a kind's schema is.
    Json kinds = Json::array();
    for (const FileKindSpec& kind : file_kinds_)
        kinds.push_back(kind.entry);
    contract.set("fileKinds", std::move(kinds));
    // Reserved section: component types populate as the declarative component compiler lands
    // (R-LANG-010); the section shape is contract from day one.
    contract.set("componentTypes", Json::array());

    Json out = Json::object();
    out.set("contract", std::move(contract));
    return out;
}

} // namespace context::editor::contract
