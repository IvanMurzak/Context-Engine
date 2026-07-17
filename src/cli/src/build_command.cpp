// `context build` (headless per-agent build) — see build_command.h.

#include "context/cli/build_command.h"

#include "context/common/subprocess.h"
#include "context/common/verify_signature.h"
#include "context/editor/build/adapter.h"
#include "context/editor/build/build_errors.h"
#include "context/editor/build/build_orchestrator.h"
#include "context/editor/build/signing.h"
#include "context/editor/build/toolchain_manifest.h"
#include "context/editor/compose/flatten.h"
#include "context/editor/compose/scene_model.h"
#include "context/editor/contract/json.h"
#include "context/editor/filesync/native_file_store.h"
#include "context/editor/filesync/path_jail.h"
#include "context/editor/pkg/tar.h"
#include "context/editor/serializer/canonical.h"

#include <cstdint>
#include <exception>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

namespace context::cli
{

using editor::contract::Envelope;
using editor::contract::Json;
namespace build = editor::build;
namespace common = context::common;
namespace compose = editor::compose;
namespace filesync = editor::filesync;
namespace serializer = editor::serializer;
namespace fs = std::filesystem;

namespace
{

[[nodiscard]] std::optional<std::string> flag(const std::map<std::string, std::string>& flags,
                                              const std::string& name)
{
    const auto it = flags.find(name);
    if (it == flags.end())
        return std::nullopt;
    return it->second;
}

// Verify-before-use of a first-party trust-bearing artifact (R-SEC-009 / L-58, task a08). Opt-in:
// verification runs ONLY when a detached signature is supplied — the per-platform export template
// (R-BUILD-004, the shipped `--runtime` host binary) and the engine-fetched/mirrored toolchain are not
// first-party-signed yet, so the gate is the READY MECHANISM wired in here, activating per acquisition
// path as it starts serving signed artifacts (docs/signing.md § Where the gate plugs in). When a
// signature IS supplied the artifact MUST verify against the pinned trust root, or the build is refused
// FAIL-CLOSED with `code`: build.template_unverified for the export template, build.toolchain_fetch_failed
// for the engine-fetched toolchain. Returns the failure Envelope on any refusal (missing artifact/root,
// tampered/unsigned/untrusted signature); std::nullopt when verification was not requested or it passed.
[[nodiscard]] std::optional<Envelope>
verify_fetched_artifact(const std::optional<std::string>& artifact,
                        const std::optional<std::string>& signature,
                        const std::optional<std::string>& trust_root, std::string_view code,
                        std::string_view role)
{
    if (!signature.has_value())
        return std::nullopt; // opt-in: no signature supplied ⇒ verification not requested (yet)
    if (!artifact.has_value() || artifact->empty())
        return Envelope::failure(std::string(code),
                                 std::string(role) +
                                     " verification was requested (a signature was supplied) but no "
                                     "artifact was given to verify (fail closed)");
    if (!trust_root.has_value() || trust_root->empty())
        return Envelope::failure(std::string(code),
                                 std::string(role) +
                                     " verification was requested but no --trust-root (pinned "
                                     "allowed_signers) was supplied (fail closed — non-TOFU root)");
    const common::VerifyOutcome v = common::verify_signature(*artifact, *signature, *trust_root);
    if (!v.ok())
        return Envelope::failure(std::string(code),
                                 std::string(role) +
                                     " failed verify-before-use against the pinned trust root "
                                     "(R-SEC-009 fail-closed): " +
                                     v.detail,
                                 *artifact);
    return std::nullopt;
}

[[nodiscard]] bool read_file(const fs::path& path, std::string& out)
{
    std::ifstream in(path, std::ios::binary);
    if (!in)
        return false;
    std::ostringstream ss;
    ss << in.rdbuf();
    out = ss.str();
    return true;
}

// Disk-backed, resolve-only scene resolver: reads + canonicalizes + composition-parses scene files
// under the project root on demand (the R-SEC-008 jail enforced lexically; the native FileStore
// re-enforces it). The resolve-only sibling of set_command's ProjectResolver — the build never writes
// authored files, so it needs no WriteResolver tree() side.
class DiskSceneResolver final : public compose::SceneResolver
{
public:
    explicit DiskSceneResolver(fs::path root) : store_(std::move(root)) {}

    [[nodiscard]] const compose::SceneDoc* resolve(std::string_view path) const override
    {
        const std::string key(path);
        if (const auto it = cache_.find(key); it != cache_.end())
            return it->second.has_doc ? &it->second.doc : nullptr;
        Entry entry;
        if (filesync::is_inside_jail(".", key))
        {
            if (const std::optional<std::string> bytes = store_.read(key))
            {
                serializer::CanonicalizeResult canonical = serializer::canonicalize(*bytes);
                if (canonical.is_json)
                {
                    if (std::optional<compose::SceneDoc> doc =
                            compose::build_scene_doc(key, canonical.root))
                    {
                        entry.has_doc = true;
                        entry.doc = std::move(*doc);
                    }
                }
            }
        }
        const auto inserted = cache_.emplace(key, std::move(entry)).first;
        return inserted->second.has_doc ? &inserted->second.doc : nullptr;
    }

private:
    struct Entry
    {
        bool has_doc = false;
        compose::SceneDoc doc;
    };
    filesync::NativeFileStore store_;
    mutable std::map<std::string, Entry, std::less<>> cache_;
};

namespace pkg = editor::pkg;
namespace subprocess = context::common::subprocess;

// The machine-readable adapter plan (a06). supported=false is the honest stub for a target with no real
// adapter yet (R-BUILD-007); a supported plan carries the flavor, render presence, runtime binary, the
// deterministic-modulo-link property, and the documented tarball layout.
[[nodiscard]] Json adapter_json(const build::AdapterPlan& plan)
{
    Json a = Json::object();
    a.set("supported", Json(plan.supported));
    a.set("stub", Json(!plan.supported));
    a.set("target", Json(plan.target));
    a.set("flavor", Json(plan.flavor));
    if (!plan.supported)
        return a;
    a.set("renderPresent", Json(plan.render_present));
    a.set("requiresSigning", Json(plan.requires_signing)); // windows Authenticode (a10); false elsewhere
    a.set("runtimeBinary", Json(plan.runtime_binary));
    if (!plan.runtime_loader.empty())
        a.set("runtimeLoader", Json(plan.runtime_loader)); // the a11 web Emscripten JS glue (web only)
    a.set("deterministicModuloLink", Json(true));
    Json layout = Json::array();
    for (const build::ArtifactEntry& e : plan.layout)
    {
        Json entry = Json::object();
        entry.set("path", Json(e.archive_path));
        entry.set("role", Json(e.role));
        layout.push_back(std::move(entry));
    }
    a.set("layout", std::move(layout));
    return a;
}

// The machine-readable signing report (a10). For a target that requires code-signing (Windows
// Authenticode, R-SEC-003) `context build --sign` folds this into the envelope: the plan (method /
// primary=Azure Trusted Signing / fallback=developer cert / mandatory timestamp) plus the OBSERVED
// state of the shipped runtime binary — "signed" when it carries an Authenticode signature, else
// "unsigned" (an EXPLICIT, never-silent WARNING with the build.artifact_unsigned code). No secret value
// ever appears here; the presence check is a pure PE parse of the runtime binary bytes.
[[nodiscard]] Json signing_json(const build::SigningReport& r)
{
    Json s = Json::object();
    s.set("required", Json(r.required));
    s.set("requested", Json(r.requested));
    s.set("state", Json(r.state));
    s.set("signed", Json(r.signed_));
    if (r.required)
    {
        s.set("method", Json(r.method));
        s.set("tool", Json(r.tool));
        s.set("primary", Json(r.primary));
        s.set("fallback", Json(r.fallback));
        s.set("timestampRequired", Json(r.timestamp_required));
    }
    if (!r.code.empty())
        s.set("code", Json(r.code));
    if (!r.warning.empty())
        s.set("warning", Json(r.warning));
    return s;
}

// Compute the signing report for `--sign`: plan the target's signing requirement, and for a required
// target read the shipped runtime binary (--runtime) and detect whether it carries an Authenticode
// signature (a pure PE parse, no signtool). A missing/unreadable runtime for a required target is a
// fail-closed "unsigned" (binary_available=false). Total — never throws.
[[nodiscard]] build::SigningReport eval_signing(const std::string& target,
                                                const std::optional<std::string>& runtime)
{
    const build::SigningPlan plan = build::plan_signing(target);
    build::SigningInputs in;
    in.requested = true; // this is only called when --sign was passed
    if (plan.required)
    {
        std::string runtime_bytes;
        in.binary_available = runtime.has_value() && !runtime->empty() &&
                              read_file(fs::path(*runtime), runtime_bytes);
        in.artifact_signed =
            in.binary_available && build::pe_has_authenticode_signature(runtime_bytes);
    }
    return build::evaluate_signing(plan, in);
}

// Assemble the runnable artifact tarball (linux/windows: bin/<runtime> + content/<pack> +
// launch.sh|launch.cmd + manifest; web (a11): bin/<wasm> + bin/<js> + index.html + content/<pack> +
// manifest) via the deterministic ustar writer (pkg::tar_write). The entries are LAYOUT-DRIVEN — one
// tar entry per plan.layout row, its bytes resolved by role — so a new adapter shape ships without a
// second assembly path. Reports emitted true/false + a reason — never a silent skip. NB: tar_write
// writes mode 0644; a consumer extracting a NATIVE tarball must `chmod +x` the runtime binary + launcher
// before running (the exec-bit is a tracked tar_write follow-up, like gzip); the web bundle's files are
// static-served, so the exec bit is irrelevant there.
[[nodiscard]] Json emit_artifact(const build::AdapterPlan& plan, const std::string& pack_bytes,
                                 std::uint64_t engine_version, std::uint64_t generation,
                                 std::uint64_t pack_hash, const std::optional<std::string>& runtime,
                                 const std::optional<std::string>& runtime_loader,
                                 const std::string& out_tar)
{
    Json e = Json::object();
    e.set("path", Json(out_tar));
    if (!plan.supported)
    {
        e.set("emitted", Json(false));
        e.set("reason", Json(std::string("no export adapter for this target/flavor")));
        return e;
    }
    // Resolve one artifact's bytes by its layout role. A "runtime"/"runtime-loader" role reads the
    // corresponding CLI-supplied file (fail-closed with a reason if absent/unreadable); the pack,
    // launcher, and manifest are pure functions already in hand. `err` is set + emit refused on failure.
    std::string err;
    const auto bytes_for = [&](const build::ArtifactEntry& entry) -> std::string {
        if (entry.role == "runtime")
        {
            if (!runtime.has_value() || runtime->empty())
            {
                err = "--emit-artifact requires --runtime <host-binary>";
                return {};
            }
            std::string b;
            if (!read_file(fs::path(*runtime), b))
                err = "could not read --runtime binary: " + *runtime;
            return b;
        }
        if (entry.role == "runtime-loader")
        {
            // The a11 web bundle's Emscripten JS glue — the second runtime file (bin/context-runtime.js).
            if (!runtime_loader.has_value() || runtime_loader->empty())
            {
                err = "--emit-artifact for a web build requires --runtime-loader <emscripten-js>";
                return {};
            }
            std::string b;
            if (!read_file(fs::path(*runtime_loader), b))
                err = "could not read --runtime-loader JS: " + *runtime_loader;
            return b;
        }
        if (entry.role == "pack")
            return pack_bytes;
        if (entry.role == "launcher")
            return build::render_launcher(plan);
        if (entry.role == "manifest")
            return build::render_manifest(plan, engine_version, generation, pack_hash);
        err = "unknown artifact layout role: " + entry.role;
        return {};
    };
    std::vector<pkg::TarEntry> entries;
    entries.reserve(plan.layout.size());
    for (const build::ArtifactEntry& entry : plan.layout)
    {
        std::string bytes = bytes_for(entry);
        if (!err.empty())
        {
            e.set("emitted", Json(false));
            e.set("reason", Json(err));
            return e;
        }
        entries.push_back({entry.archive_path, std::move(bytes), false});
    }
    const std::optional<std::string> tar = pkg::tar_write(entries);
    if (!tar.has_value())
    {
        e.set("emitted", Json(false));
        e.set("reason", Json(std::string("tar assembly failed (a path exceeded the ustar limit)")));
        return e;
    }
    std::error_code ec;
    const fs::path out_fs(out_tar);
    if (out_fs.has_parent_path())
        fs::create_directories(out_fs.parent_path(), ec);
    std::ofstream os(out_fs, std::ios::binary | std::ios::trunc);
    if (!os || !os.write(tar->data(), static_cast<std::streamsize>(tar->size())))
    {
        e.set("emitted", Json(false));
        e.set("reason", Json("could not write artifact tarball: " + out_tar));
        return e;
    }
    e.set("emitted", Json(true));
    e.set("entryCount", Json(static_cast<std::uint64_t>(entries.size())));
    return e;
}

// Launch the produced artifact against the shipped runtime binary (R-BUILD-009) and fold its boot/state
// signal into the envelope. A target that cannot be smoke-run declares ran=false + a reason (never a
// silent skip). The child's stdout is captured through a scratch file (the subprocess runner returns
// only the exit code); its JSON signal is parsed and surfaced.
[[nodiscard]] Json run_smoke(const build::AdapterPlan& plan, const std::optional<std::string>& runtime,
                             const std::string& pack_path, bool dry_run, int ticks)
{
    Json sm = Json::object();
    const auto not_run = [&sm](const std::string& reason) {
        sm.set("ran", Json(false));
        sm.set("reason", Json(reason));
        return sm;
    };
    if (!plan.supported)
        return not_run("no export adapter for this target/flavor");
    // The a11 web bundle boots in a browser (Emscripten/WebGPU), not against the native runtime — its
    // smoke gate is the render-web CI job (headless Chromium + SwiftShader), never this native launch.
    if (plan.launcher_kind == build::LauncherKind::WebHtml)
        return not_run("web target boots in a browser — the render-web CI job (headless Chromium) is "
                       "its smoke gate, not the native runtime");
    if (!runtime.has_value() || runtime->empty())
        return not_run("--smoke requires --runtime <host-binary>");
    if (dry_run)
        return not_run("--dry-run wrote no pack to smoke-run");
    if (!fs::exists(fs::path(pack_path)))
        return not_run("the built pack is not present at " + pack_path);

    const fs::path out = subprocess::make_scratch_path("ctx-build-smoke", ".json");
    subprocess::ScratchFile out_guard(out);
    std::string cmd;
    try
    {
        cmd = subprocess::quote_argument(*runtime) + " --pack " +
              subprocess::quote_argument(pack_path) + " --ticks " + std::to_string(ticks) + " > " +
              subprocess::quote_argument(out.string());
    }
    catch (const subprocess::MetacharacterError& ex)
    {
        return not_run(std::string("could not build a safe launch command: ") + ex.what());
    }
    const int exit_code = subprocess::run_command(cmd);
    const std::string signal = subprocess::read_file(out);

    sm.set("ran", Json(true));
    sm.set("ticks", Json(static_cast<std::uint64_t>(ticks < 0 ? 0 : ticks)));
    sm.set("exitCode", Json(static_cast<std::int64_t>(exit_code)));
    // Parse the runtime's boot/state signal (best-effort — a launch failure yields empty stdout).
    try
    {
        const Json parsed = Json::parse(signal);
        sm.set("ok", Json(exit_code == 0 && parsed.contains("ok") && parsed.at("ok").as_bool()));
        if (parsed.contains("simTick"))
            sm.set("simTick", Json(static_cast<std::uint64_t>(parsed.at("simTick").as_int())));
        if (parsed.contains("rootScene"))
            sm.set("rootScene", Json(parsed.at("rootScene").as_string()));
        if (parsed.contains("flavor"))
            sm.set("flavor", Json(parsed.at("flavor").as_string()));
        if (parsed.contains("renderPresent"))
            sm.set("renderPresent", Json(parsed.at("renderPresent").as_bool()));
    }
    catch (const std::exception&)
    {
        // The runtime emitted no parseable signal (e.g. it could not be launched on this host).
        sm.set("ok", Json(false));
        sm.set("reason", Json(std::string("the runtime emitted no parseable boot/state signal")));
    }
    return sm;
}

} // namespace

Envelope run_build(const std::map<std::string, std::string>& flags)
{
    const std::string target = flag(flags, "target").value_or("");
    if (target.empty())
        return Envelope::failure("usage.missing_argument",
                                 "context build requires --target <windows|linux|macos|web>");
    const std::string project = flag(flags, "project").value_or(".");
    const bool dry_run = flags.find("dry-run") != flags.end();

    // The a06 export flavor (desktop = render present; server = headless, render absent). Default
    // desktop. A malformed value is a usage error, not a silent fallback (R-CLI-007).
    const std::string flavor = flag(flags, "flavor").value_or(build::kFlavorDesktop);
    if (!build::is_known_flavor(flavor))
        return Envelope::failure("usage.invalid",
                                 "unknown --flavor '" + flavor + "' (expected desktop | server)",
                                 flavor);

    // Read the project manifest → the root scene, plus the OPTIONAL scripts / packages declarations the
    // build consumes (absent ⇒ a data-only game: no AOT tier, no linked packages). A missing/malformed
    // manifest is a pre-build template failure.
    std::string manifest_text;
    if (!read_file(fs::path(project) / "project.json", manifest_text))
        return Envelope::failure(std::string(build::kBuildTemplateUnverifiedCode),
                                 "project.json not found under " + project, "project.json");

    build::BuildRequest req;
    req.target = target;
    req.flavor = flavor;
    req.toolchain = build::toolchain_manifest();
    DiskSceneResolver resolver(project);
    req.resolver = &resolver;
    try
    {
        const Json manifest = Json::parse(manifest_text);
        req.root_scene_path = manifest.at("scene").as_string();
        if (manifest.contains("scripts") && manifest.at("scripts").is_array())
        {
            const Json& scripts = manifest.at("scripts");
            for (std::size_t i = 0; i < scripts.size(); ++i)
            {
                const Json& s = scripts.at(i);
                build::BuildScript bs;
                bs.name = s.contains("name") ? s.at("name").as_string() : std::string();
                bs.entrypoint = s.contains("entrypoint") ? s.at("entrypoint").as_string()
                                                         : std::string();
                req.scripts.push_back(std::move(bs));
            }
        }
        if (manifest.contains("packages") && manifest.at("packages").is_array())
        {
            const Json& pkgs = manifest.at("packages");
            for (std::size_t i = 0; i < pkgs.size(); ++i)
                req.referenced_packages.push_back(pkgs.at(i).as_string());
        }
    }
    catch (const std::exception& e)
    {
        return Envelope::failure(std::string(build::kBuildTemplateUnverifiedCode),
                                 std::string("project.json is malformed: ") + e.what(),
                                 "project.json");
    }

    // The engine's registrable first-party package set (packages with a register_<pkg> Module —
    // src/packages/). A manifest referencing anything outside this set is an undefined register_<pkg>
    // (build.link_failed). The default template references none, so its link is a no-op.
    req.registrable_packages = {"spatial",   "simmath",  "physics3d", "physics2d", "particles",
                                "animation", "spline",   "audio",     "input",     "ui"};

    // Verify-before-use of the engine-fetched (mirrored) toolchain artifact (R-SEC-009 / R-PKG-002),
    // when a detached signature is supplied. A first-party toolchain artifact that does not verify
    // against the pinned trust root cannot be trusted to build with — refuse fail-closed with
    // build.toolchain_fetch_failed BEFORE building (an unverifiable fetch is a failed fetch).
    if (std::optional<Envelope> refusal = verify_fetched_artifact(
            flag(flags, "toolchain-artifact"), flag(flags, "toolchain-sig"), flag(flags, "trust-root"),
            build::kBuildToolchainFetchFailedCode, "the engine-fetched toolchain"))
        return *refusal;

    const build::BuildResult result = build::run_build(req);
    if (!result.ok)
    {
        std::optional<std::string> pointer;
        if (!result.error_pointer.empty())
            pointer = result.error_pointer;
        return Envelope::failure(result.error_code, result.error_message, pointer);
    }

    const std::string out_path =
        flag(flags, "out")
            .value_or((fs::path(project) / "build" / (target + std::string(".pack"))).generic_string());

    if (!dry_run)
    {
        const fs::path out_fs(out_path);
        std::error_code ec;
        if (out_fs.has_parent_path())
            fs::create_directories(out_fs.parent_path(), ec);
        std::ofstream os(out_fs, std::ios::binary | std::ios::trunc);
        if (!os)
            return Envelope::failure("internal.error", "could not open output pack path: " + out_path,
                                     out_path);
        os.write(result.pack_bytes.data(), static_cast<std::streamsize>(result.pack_bytes.size()));
        if (!os)
            return Envelope::failure("internal.error", "could not write output pack: " + out_path,
                                     out_path);
    }

    const build::BuildSummary& s = result.summary;
    Json data = Json::object();
    data.set("target", Json(s.target));
    data.set("dryRun", Json(dry_run));
    data.set("engineVersion", Json(static_cast<std::uint64_t>(s.engine_version)));
    // The build's derived-world generation — a deterministic 64-bit content identity of the world this
    // pack was built from (reproducible: the same project always yields the same generation). Reported
    // as a DECIMAL STRING because the R-CLI-008 envelope's JSON numbers are doubles, which would lose
    // precision above 2^53 (the same reason $sidecar hashes / L-33 ids are strings).
    data.set("generation", Json(std::to_string(s.generation)));

    Json artifact = Json::object();
    artifact.set("packPath", Json(out_path));
    artifact.set("written", Json(!dry_run));
    artifact.set("packHash", Json(std::to_string(s.pack_hash))); // 64-bit fingerprint — decimal string
    artifact.set("packSize", Json(static_cast<std::uint64_t>(s.pack_size)));
    artifact.set("unitCount", Json(static_cast<std::uint64_t>(s.unit_count)));
    artifact.set("chunkCount", Json(static_cast<std::uint64_t>(s.chunk_count)));
    artifact.set("sidecarCount", Json(static_cast<std::uint64_t>(s.sidecar_count)));
    artifact.set("entityCount", Json(static_cast<std::uint64_t>(s.entity_count)));
    data.set("artifact", std::move(artifact));

    Json registered = Json::array();
    for (const std::string& p : s.registered_packages)
        registered.push_back(Json(p));
    data.set("registeredPackages", std::move(registered));

    // The a06 export adapter (Linux desktop + server/headless are real; other targets report the honest
    // stub — R-BUILD-007). The plan is machine-readable in the envelope: an autonomous agent shipping
    // multiple platforms reads whether a runnable artifact is available for (target, flavor).
    data.set("adapter", adapter_json(s.adapter));

    // Verify-before-use of the R-BUILD-004 export template — the shipped runtime host binary the
    // runnable artifact is assembled from (--runtime) — when a detached signature (--runtime-sig) is
    // supplied. An export template that does not verify against the pinned trust root is refused
    // fail-closed with build.template_unverified BEFORE it is packed into a runnable artifact.
    if (std::optional<Envelope> refusal = verify_fetched_artifact(
            flag(flags, "runtime"), flag(flags, "runtime-sig"), flag(flags, "trust-root"),
            build::kBuildTemplateUnverifiedCode, "the export template (--runtime host binary)"))
        return *refusal;

    // --emit-artifact: assemble the runnable artifact tarball (R-BUILD-005 minimal packaging) — the
    // shipped runtime binary (--runtime) + this pack + launch.sh + the manifest. Reported machine-
    // readably (emitted true/false + reason), never a silent skip.
    const std::optional<std::string> emit_path = flag(flags, "emit-artifact");
    if (emit_path.has_value())
        data.set("artifactEmit",
                 emit_artifact(s.adapter, result.pack_bytes, s.engine_version, s.generation,
                               s.pack_hash, flag(flags, "runtime"), flag(flags, "runtime-loader"),
                               *emit_path));

    // --sign (a10, R-SEC-003): the Authenticode signing hook. Fold the machine-readable signing report
    // (plan + observed state of the shipped runtime binary) into the envelope. For a signing-required
    // target (Windows) whose runtime binary is NOT signed, this is an EXPLICIT never-silent WARNING
    // (data.signing.state == "unsigned", the build.artifact_unsigned code, AND an envelope warning) —
    // NOT a build failure (the artifact is produced; it just needs signing before it ships).
    std::string signing_warning;
    if (flags.find("sign") != flags.end())
    {
        const build::SigningReport report = eval_signing(target, flag(flags, "runtime"));
        data.set("signing", signing_json(report));
        signing_warning = report.warning; // "" unless required-but-unsigned
    }

    // --smoke (R-BUILD-009): launch the produced artifact against the shipped runtime binary, step N
    // fixed ticks, and fold the boot/state signal into the envelope. A target that cannot be smoke-run
    // (no adapter, no --runtime, or nothing written) declares that machine-readably rather than skipping.
    if (flags.find("smoke") != flags.end())
    {
        int smoke_ticks = 8;
        if (const std::optional<std::string> t = flag(flags, "smoke-ticks"); t.has_value())
        {
            try
            {
                smoke_ticks = std::stoi(*t);
            }
            catch (const std::exception&)
            {
                return Envelope::failure("usage.invalid",
                                         "--smoke-ticks must be a non-negative integer", *t);
            }
            // A negative value parses cleanly but is invalid: it would reach the runtime as `--ticks -N`,
            // where std::stoull wraps it to a huge tick count → the smoke child hangs. Reject it here so
            // the "non-negative integer" contract above holds for real.
            if (smoke_ticks < 0)
                return Envelope::failure("usage.invalid",
                                         "--smoke-ticks must be a non-negative integer", *t);
        }
        data.set("smoke", run_smoke(s.adapter, flag(flags, "runtime"), out_path, dry_run, smoke_ticks));
    }

    // A build mutates no authored file, so the envelope's generationAfter (the derived-world generation
    // a WRITE lands in) is 0; the build's own content generation is reported in data above.
    Envelope env = Envelope::success(std::move(data), 0);
    // The signing hook's never-silent warning: an unsigned artifact for a signing-required target
    // surfaces in the envelope warnings[] too (not only data.signing) — a legible, top-level signal.
    if (!signing_warning.empty())
        env.add_warning(signing_warning);
    return env;
}

} // namespace context::cli
