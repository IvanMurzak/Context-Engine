// GENERATED FILE - DO NOT EDIT BY HAND.
//
// Projected from the contract registry's client schema by tools/gen_client_typings.py:
//   contract::Registry::describe -> context_client_schema_gen (e02) -> context-client-schema.json
//   -> gen_client_typings.py -> this file.
//
// Hand-written client typings are prohibited (design 05 section 3, the R-CLI-009 spirit): the
// registry is the single source of truth. Regenerate with
//   python3 tools/gen_client_typings.py
// The `webui-client-typings-drift` ctest re-runs the generator against the BUILD-generated schema
// and fails on any byte difference, so this file can never go stale against the daemon's surface.


/** The frozen wire protocol major (R-CLI-004). */
export const PROTOCOL_MAJOR = 1 as const;

/** Oldest client protocol the daemon still accepts. */
export const MIN_CLIENT_PROTOCOL = 1 as const;

/** Negotiated protocol capabilities advertised by the daemon. */
export const PROTOCOL_CAPABILITIES = ["describe", "result-envelope", "verb-grammar", "dry-run", "if-match"] as const;
export type ProtocolCapability = (typeof PROTOCOL_CAPABILITIES)[number];

/** Schema version this module was projected from. */
export const CLIENT_SCHEMA_VERSION = 1 as const;

/** Every RPC method the contract registry publishes. */
export type RpcMethod =
    | "describe"
    | "new"
    | "set"
    | "migrate"
    | "package.add"
    | "resource.read"
    | "asset.move"
    | "asset.rename"
    | "merge-file"
    | "resolve-conflict"
    | "re-key"
    | "validate"
    | "session.new"
    | "session.step"
    | "session.seed"
    | "session.inject"
    | "session.hash"
    | "session.record"
    | "replay"
    | "determinism.diff"
    | "install"
    | "profile.gc"
    | "profile.session"
    | "ui.dump"
    | "ui.query"
    | "ui.send"
    | "ui.assert"
    | "tilemap.paint"
    | "tilemap.fill"
    | "build"
    | "doctor"
    | "edit"
    | "edit-batch"
    | "query"
    | "snapshot"
    | "editor.scene-tree"
    | "editor.inspect"
    | "editor.select"
    | "editor.selection-get"
    | "editor.camera-set"
    | "editor.cameras-get"
    | "editor.play"
    | "editor.pause"
    | "editor.stop"
    | "editor.step"
    | "subscribe"
    | "unsubscribe"
    | "ack"
    | "reconcile"
    | "shutdown"
    | "debug.attach";

/** A registry-projected description of one RPC method. */
export interface RpcMethodDescriptor {
    readonly method: RpcMethod;
    /** Verb-grammar namespace (empty for top-level verbs). */
    readonly ns: string;
    /** Verb-grammar noun (empty for top-level verbs). */
    readonly noun: string;
    readonly verb: string;
    readonly stability: string;
    readonly deprecated: boolean;
    /** Positional parameter names, in grammar order. */
    readonly params: readonly string[];
    /** Flag names accepted by this method. */
    readonly flags: readonly string[];
}

export const RPC_METHODS: { readonly [M in RpcMethod]: RpcMethodDescriptor } = {
    "describe": {
        method: "describe",
        ns: "",
        noun: "",
        verb: "describe",
        stability: "stable",
        deprecated: false,
        params: [],
        flags: ["json", "project", "if-match", "after-generation", "dry-run", "idempotency-key", "after-hash", "atomic-plan"],
    },
    "new": {
        method: "new",
        ns: "",
        noun: "",
        verb: "new",
        stability: "stable",
        deprecated: false,
        params: ["directory", "template"],
        flags: ["json", "project", "if-match", "after-generation", "dry-run", "idempotency-key", "after-hash", "atomic-plan"],
    },
    "set": {
        method: "set",
        ns: "",
        noun: "",
        verb: "set",
        stability: "stable",
        deprecated: false,
        params: ["path", "value"],
        flags: ["json", "project", "if-match", "after-generation", "dry-run", "idempotency-key", "after-hash", "atomic-plan", "pointer", "id-path", "edit-template", "at-instance"],
    },
    "migrate": {
        method: "migrate",
        ns: "",
        noun: "",
        verb: "migrate",
        stability: "stable",
        deprecated: false,
        params: ["path"],
        flags: ["json", "project", "if-match", "after-generation", "dry-run", "idempotency-key", "after-hash", "atomic-plan"],
    },
    "package.add": {
        method: "package.add",
        ns: "",
        noun: "package",
        verb: "add",
        stability: "stable",
        deprecated: false,
        params: ["name"],
        flags: ["json", "project", "if-match", "after-generation", "dry-run", "idempotency-key", "after-hash", "atomic-plan"],
    },
    "resource.read": {
        method: "resource.read",
        ns: "",
        noun: "resource",
        verb: "read",
        stability: "stable",
        deprecated: false,
        params: ["handle", "range"],
        flags: ["json", "project", "if-match", "after-generation", "dry-run", "idempotency-key", "after-hash", "atomic-plan", "out"],
    },
    "asset.move": {
        method: "asset.move",
        ns: "",
        noun: "asset",
        verb: "move",
        stability: "stable",
        deprecated: false,
        params: ["from", "to"],
        flags: ["json", "project", "if-match", "after-generation", "dry-run", "idempotency-key", "after-hash", "atomic-plan"],
    },
    "asset.rename": {
        method: "asset.rename",
        ns: "",
        noun: "asset",
        verb: "rename",
        stability: "stable",
        deprecated: false,
        params: ["from", "to"],
        flags: ["json", "project", "if-match", "after-generation", "dry-run", "idempotency-key", "after-hash", "atomic-plan"],
    },
    "merge-file": {
        method: "merge-file",
        ns: "",
        noun: "",
        verb: "merge-file",
        stability: "stable",
        deprecated: false,
        params: ["base", "ours", "theirs", "pathname"],
        flags: ["json", "project", "if-match", "after-generation", "dry-run", "idempotency-key", "after-hash", "atomic-plan", "output", "driver"],
    },
    "resolve-conflict": {
        method: "resolve-conflict",
        ns: "",
        noun: "",
        verb: "resolve-conflict",
        stability: "stable",
        deprecated: false,
        params: ["file"],
        flags: ["json", "project", "if-match", "after-generation", "dry-run", "idempotency-key", "after-hash", "atomic-plan", "path", "take", "value"],
    },
    "re-key": {
        method: "re-key",
        ns: "",
        noun: "",
        verb: "re-key",
        stability: "stable",
        deprecated: false,
        params: ["file"],
        flags: ["json", "project", "if-match", "after-generation", "dry-run", "idempotency-key", "after-hash", "atomic-plan", "at", "id"],
    },
    "validate": {
        method: "validate",
        ns: "",
        noun: "",
        verb: "validate",
        stability: "stable",
        deprecated: false,
        params: ["path"],
        flags: ["json", "project", "if-match", "after-generation", "dry-run", "idempotency-key", "after-hash", "atomic-plan"],
    },
    "session.new": {
        method: "session.new",
        ns: "",
        noun: "session",
        verb: "new",
        stability: "stable",
        deprecated: false,
        params: ["state"],
        flags: ["json", "project", "if-match", "after-generation", "dry-run", "idempotency-key", "after-hash", "atomic-plan", "seed", "scenario"],
    },
    "session.step": {
        method: "session.step",
        ns: "",
        noun: "session",
        verb: "step",
        stability: "stable",
        deprecated: false,
        params: ["state"],
        flags: ["json", "project", "if-match", "after-generation", "dry-run", "idempotency-key", "after-hash", "atomic-plan", "ticks", "trace"],
    },
    "session.seed": {
        method: "session.seed",
        ns: "",
        noun: "session",
        verb: "seed",
        stability: "stable",
        deprecated: false,
        params: ["state"],
        flags: ["json", "project", "if-match", "after-generation", "dry-run", "idempotency-key", "after-hash", "atomic-plan", "set"],
    },
    "session.inject": {
        method: "session.inject",
        ns: "",
        noun: "session",
        verb: "inject",
        stability: "stable",
        deprecated: false,
        params: ["state"],
        flags: ["json", "project", "if-match", "after-generation", "dry-run", "idempotency-key", "after-hash", "atomic-plan", "action", "event", "code", "phase", "value", "at"],
    },
    "session.hash": {
        method: "session.hash",
        ns: "",
        noun: "session",
        verb: "hash",
        stability: "stable",
        deprecated: false,
        params: ["state"],
        flags: ["json", "project", "if-match", "after-generation", "dry-run", "idempotency-key", "after-hash", "atomic-plan"],
    },
    "session.record": {
        method: "session.record",
        ns: "",
        noun: "session",
        verb: "record",
        stability: "stable",
        deprecated: false,
        params: ["state"],
        flags: ["json", "project", "if-match", "after-generation", "dry-run", "idempotency-key", "after-hash", "atomic-plan", "out", "manifest", "non-deterministic"],
    },
    "replay": {
        method: "replay",
        ns: "",
        noun: "",
        verb: "replay",
        stability: "stable",
        deprecated: false,
        params: ["artifact"],
        flags: ["json", "project", "if-match", "after-generation", "dry-run", "idempotency-key", "after-hash", "atomic-plan"],
    },
    "determinism.diff": {
        method: "determinism.diff",
        ns: "",
        noun: "determinism",
        verb: "diff",
        stability: "stable",
        deprecated: false,
        params: ["left", "right"],
        flags: ["json", "project", "if-match", "after-generation", "dry-run", "idempotency-key", "after-hash", "atomic-plan"],
    },
    "install": {
        method: "install",
        ns: "",
        noun: "",
        verb: "install",
        stability: "stable",
        deprecated: false,
        params: [],
        flags: ["json", "project", "if-match", "after-generation", "dry-run", "idempotency-key", "after-hash", "atomic-plan", "source", "production"],
    },
    "profile.gc": {
        method: "profile.gc",
        ns: "",
        noun: "profile",
        verb: "gc",
        stability: "stable",
        deprecated: false,
        params: [],
        flags: ["json", "project", "if-match", "after-generation", "dry-run", "idempotency-key", "after-hash", "atomic-plan", "ticks", "budget-ms", "trigger-bytes", "churn"],
    },
    "profile.session": {
        method: "profile.session",
        ns: "",
        noun: "profile",
        verb: "session",
        stability: "stable",
        deprecated: false,
        params: [],
        flags: ["json", "project", "if-match", "after-generation", "dry-run", "idempotency-key", "after-hash", "atomic-plan", "ticks", "churn", "trace-out"],
    },
    "ui.dump": {
        method: "ui.dump",
        ns: "",
        noun: "ui",
        verb: "dump",
        stability: "stable",
        deprecated: false,
        params: ["scene"],
        flags: ["json", "project", "if-match", "after-generation", "dry-run", "idempotency-key", "after-hash", "atomic-plan", "width", "height"],
    },
    "ui.query": {
        method: "ui.query",
        ns: "",
        noun: "ui",
        verb: "query",
        stability: "stable",
        deprecated: false,
        params: ["scene", "node"],
        flags: ["json", "project", "if-match", "after-generation", "dry-run", "idempotency-key", "after-hash", "atomic-plan", "width", "height"],
    },
    "ui.send": {
        method: "ui.send",
        ns: "",
        noun: "ui",
        verb: "send",
        stability: "stable",
        deprecated: false,
        params: ["scene", "event"],
        flags: ["json", "project", "if-match", "after-generation", "dry-run", "idempotency-key", "after-hash", "atomic-plan", "target", "code", "text", "width", "height"],
    },
    "ui.assert": {
        method: "ui.assert",
        ns: "",
        noun: "ui",
        verb: "assert",
        stability: "stable",
        deprecated: false,
        params: ["scene", "node"],
        flags: ["json", "project", "if-match", "after-generation", "dry-run", "idempotency-key", "after-hash", "atomic-plan", "exists", "role", "text", "value", "visible", "hidden", "child-count"],
    },
    "tilemap.paint": {
        method: "tilemap.paint",
        ns: "",
        noun: "tilemap",
        verb: "paint",
        stability: "stable",
        deprecated: false,
        params: ["path", "cells"],
        flags: ["json", "project", "if-match", "after-generation", "dry-run", "idempotency-key", "after-hash", "atomic-plan", "layer"],
    },
    "tilemap.fill": {
        method: "tilemap.fill",
        ns: "",
        noun: "tilemap",
        verb: "fill",
        stability: "stable",
        deprecated: false,
        params: ["path", "tile"],
        flags: ["json", "project", "if-match", "after-generation", "dry-run", "idempotency-key", "after-hash", "atomic-plan", "layer", "rect"],
    },
    "build": {
        method: "build",
        ns: "",
        noun: "",
        verb: "build",
        stability: "stable",
        deprecated: false,
        params: [],
        flags: ["json", "project", "if-match", "after-generation", "dry-run", "idempotency-key", "after-hash", "atomic-plan", "target", "out", "flavor", "runtime", "runtime-loader", "emit-artifact", "smoke", "smoke-ticks", "sign", "trust-root", "runtime-sig", "toolchain-artifact", "toolchain-sig"],
    },
    "doctor": {
        method: "doctor",
        ns: "",
        noun: "",
        verb: "doctor",
        stability: "stable",
        deprecated: false,
        params: [],
        flags: ["json", "project", "if-match", "after-generation", "dry-run", "idempotency-key", "after-hash", "atomic-plan", "target", "fetch"],
    },
    "edit": {
        method: "edit",
        ns: "",
        noun: "",
        verb: "edit",
        stability: "operational",
        deprecated: false,
        params: ["path", "content"],
        flags: ["json", "project", "if-match", "after-generation", "dry-run", "idempotency-key", "after-hash", "atomic-plan"],
    },
    "edit-batch": {
        method: "edit-batch",
        ns: "",
        noun: "",
        verb: "edit-batch",
        stability: "operational",
        deprecated: false,
        params: ["files"],
        flags: ["json", "project", "if-match", "after-generation", "dry-run", "idempotency-key", "after-hash", "atomic-plan"],
    },
    "query": {
        method: "query",
        ns: "",
        noun: "",
        verb: "query",
        stability: "operational",
        deprecated: false,
        params: ["path"],
        flags: ["json", "project", "if-match", "after-generation", "dry-run", "idempotency-key", "after-hash", "atomic-plan", "overrides"],
    },
    "snapshot": {
        method: "snapshot",
        ns: "",
        noun: "",
        verb: "snapshot",
        stability: "operational",
        deprecated: false,
        params: [],
        flags: ["json", "project", "if-match", "after-generation", "dry-run", "idempotency-key", "after-hash", "atomic-plan"],
    },
    "editor.scene-tree": {
        method: "editor.scene-tree",
        ns: "",
        noun: "editor",
        verb: "scene-tree",
        stability: "operational",
        deprecated: false,
        params: ["path"],
        flags: ["json", "project", "if-match", "after-generation", "dry-run", "idempotency-key", "after-hash", "atomic-plan"],
    },
    "editor.inspect": {
        method: "editor.inspect",
        ns: "",
        noun: "editor",
        verb: "inspect",
        stability: "operational",
        deprecated: false,
        params: ["path", "idPath"],
        flags: ["json", "project", "if-match", "after-generation", "dry-run", "idempotency-key", "after-hash", "atomic-plan"],
    },
    "editor.select": {
        method: "editor.select",
        ns: "",
        noun: "editor",
        verb: "select",
        stability: "operational",
        deprecated: false,
        params: ["ids", "mode"],
        flags: ["json", "project", "if-match", "after-generation", "dry-run", "idempotency-key", "after-hash", "atomic-plan"],
    },
    "editor.selection-get": {
        method: "editor.selection-get",
        ns: "",
        noun: "editor",
        verb: "selection-get",
        stability: "operational",
        deprecated: false,
        params: [],
        flags: ["json", "project", "if-match", "after-generation", "dry-run", "idempotency-key", "after-hash", "atomic-plan"],
    },
    "editor.camera-set": {
        method: "editor.camera-set",
        ns: "",
        noun: "editor",
        verb: "camera-set",
        stability: "operational",
        deprecated: false,
        params: ["viewportId", "transform", "projection"],
        flags: ["json", "project", "if-match", "after-generation", "dry-run", "idempotency-key", "after-hash", "atomic-plan"],
    },
    "editor.cameras-get": {
        method: "editor.cameras-get",
        ns: "",
        noun: "editor",
        verb: "cameras-get",
        stability: "operational",
        deprecated: false,
        params: [],
        flags: ["json", "project", "if-match", "after-generation", "dry-run", "idempotency-key", "after-hash", "atomic-plan"],
    },
    "editor.play": {
        method: "editor.play",
        ns: "",
        noun: "editor",
        verb: "play",
        stability: "operational",
        deprecated: false,
        params: [],
        flags: ["json", "project", "if-match", "after-generation", "dry-run", "idempotency-key", "after-hash", "atomic-plan"],
    },
    "editor.pause": {
        method: "editor.pause",
        ns: "",
        noun: "editor",
        verb: "pause",
        stability: "operational",
        deprecated: false,
        params: [],
        flags: ["json", "project", "if-match", "after-generation", "dry-run", "idempotency-key", "after-hash", "atomic-plan"],
    },
    "editor.stop": {
        method: "editor.stop",
        ns: "",
        noun: "editor",
        verb: "stop",
        stability: "operational",
        deprecated: false,
        params: [],
        flags: ["json", "project", "if-match", "after-generation", "dry-run", "idempotency-key", "after-hash", "atomic-plan"],
    },
    "editor.step": {
        method: "editor.step",
        ns: "",
        noun: "editor",
        verb: "step",
        stability: "operational",
        deprecated: false,
        params: [],
        flags: ["json", "project", "if-match", "after-generation", "dry-run", "idempotency-key", "after-hash", "atomic-plan", "ticks"],
    },
    "subscribe": {
        method: "subscribe",
        ns: "",
        noun: "",
        verb: "subscribe",
        stability: "operational",
        deprecated: false,
        params: ["topics", "pathScope", "sinceSeq"],
        flags: ["json", "project", "if-match", "after-generation", "dry-run", "idempotency-key", "after-hash", "atomic-plan"],
    },
    "unsubscribe": {
        method: "unsubscribe",
        ns: "",
        noun: "",
        verb: "unsubscribe",
        stability: "operational",
        deprecated: false,
        params: ["subId"],
        flags: ["json", "project", "if-match", "after-generation", "dry-run", "idempotency-key", "after-hash", "atomic-plan"],
    },
    "ack": {
        method: "ack",
        ns: "",
        noun: "",
        verb: "ack",
        stability: "operational",
        deprecated: false,
        params: ["subId", "seq"],
        flags: ["json", "project", "if-match", "after-generation", "dry-run", "idempotency-key", "after-hash", "atomic-plan"],
    },
    "reconcile": {
        method: "reconcile",
        ns: "",
        noun: "",
        verb: "reconcile",
        stability: "operational",
        deprecated: false,
        params: [],
        flags: ["json", "project", "if-match", "after-generation", "dry-run", "idempotency-key", "after-hash", "atomic-plan"],
    },
    "shutdown": {
        method: "shutdown",
        ns: "",
        noun: "",
        verb: "shutdown",
        stability: "operational",
        deprecated: false,
        params: [],
        flags: ["json", "project", "if-match", "after-generation", "dry-run", "idempotency-key", "after-hash", "atomic-plan"],
    },
    "debug.attach": {
        method: "debug.attach",
        ns: "",
        noun: "debug",
        verb: "attach",
        stability: "operational",
        deprecated: false,
        params: [],
        flags: ["json", "project", "if-match", "after-generation", "dry-run", "idempotency-key", "after-hash", "atomic-plan", "endpoint"],
    },
};

/** Every RPC method name, in registry order. */
export const RPC_METHOD_NAMES: readonly RpcMethod[] = ["describe", "new", "set", "migrate", "package.add", "resource.read", "asset.move", "asset.rename", "merge-file", "resolve-conflict", "re-key", "validate", "session.new", "session.step", "session.seed", "session.inject", "session.hash", "session.record", "replay", "determinism.diff", "install", "profile.gc", "profile.session", "ui.dump", "ui.query", "ui.send", "ui.assert", "tilemap.paint", "tilemap.fill", "build", "doctor", "edit", "edit-batch", "query", "snapshot", "editor.scene-tree", "editor.inspect", "editor.select", "editor.selection-get", "editor.camera-set", "editor.cameras-get", "editor.play", "editor.pause", "editor.stop", "editor.step", "subscribe", "unsubscribe", "ack", "reconcile", "shutdown", "debug.attach"];

/** Every subscribable event topic. */
export type EventTopic =
    | "files"
    | "derivation"
    | "diagnostics"
    | "session"
    | "clients"
    | "log";

/** One field of a topic's payload, as described by the registry. */
export interface PayloadField {
    readonly name: string;
    readonly type: string;
    readonly description: string;
}

/** A registry-projected description of one event topic. */
export interface EventTopicDescriptor {
    readonly name: EventTopic;
    readonly description: string;
    readonly payloadFields: readonly PayloadField[];
}

export const EVENT_TOPICS: { readonly [T in EventTopic]: EventTopicDescriptor } = {
    "files": {
        name: "files",
        description: "Post-derivation file change facts (never raw filesystem noise).",
        payloadFields: [
            { name: "path", type: "path", description: "The project-relative file the fact is about." },
            { name: "change", type: "string", description: "The change class: added | modified | removed." },
        ],
    },
    "derivation": {
        name: "derivation",
        description: "Derived-world updates, each stamped with the input content-hash.",
        payloadFields: [
            { name: "event", type: "string", description: "The derivation event, e.g. derivation.settled." },
            { name: "generation", type: "generation", description: "The derived-world generation this update reflects." },
            { name: "inputHash", type: "hash", description: "The content-hash of the input file version derived from (the write-acknowledgement)." },
        ],
    },
    "diagnostics": {
        name: "diagnostics",
        description: "Machine-readable diagnostics through the R-CLI-008 error schema.",
        payloadFields: [
            { name: "code", type: "string", description: "The R-CLI-008 catalog error code." },
            { name: "message", type: "string", description: "The human-readable diagnostic message." },
            { name: "stability", type: "string", description: "provisional while the generation churns; promoted to stable once it settles (R-BRIDGE-008)." },
            { name: "pointer", type: "string", description: "Optional RFC 6901 JSON-pointer / location the diagnostic is about." },
        ],
    },
    "session": {
        name: "session",
        description: "Session lifecycle + restart-class reload announcements, and the editor session-state facts (selection / cameras / play) every client shares (M9 D7).",
        payloadFields: [
            { name: "event", type: "string", description: "The event: started | reloaded | stopped (lifecycle), or selection-changed | camera-changed | play-state (editor session state)." },
            { name: "origin", type: "generation", description: "Echo suppression: the id of the CLIENT whose call produced this fact (the value `attach` returned as clientId); 0 = the daemon itself." },
            { name: "ids", type: "json", description: "selection-changed: the L-35 id-paths now selected (the whole selection, not a delta)." },
            { name: "mode", type: "string", description: "selection-changed: how the change was applied \u2014 replace | add | toggle | remove." },
            { name: "viewportId", type: "string", description: "camera-changed: the viewport whose camera changed." },
            { name: "state", type: "string", description: "play-state: the L-51 provenance state after the transition \u2014 edit | playing | paused." },
            { name: "simTick", type: "generation", description: "play-state: the live session's fixed-tick counter (0 in edit state)." },
        ],
    },
    "clients": {
        name: "clients",
        description: "Client attach/detach on the daemon.",
        payloadFields: [
            { name: "event", type: "string", description: "attached | detached." },
            { name: "protocolMajor", type: "generation", description: "The negotiated protocol major (on attach)." },
            { name: "scopes", type: "json", description: "The granted scope names (on attach)." },
        ],
    },
    "log": {
        name: "log",
        description: "Structured log entries (severity, source, tick, session).",
        payloadFields: [
            { name: "level", type: "string", description: "The severity: trace | debug | info | warn | error." },
            { name: "message", type: "string", description: "The log message text." },
            { name: "source", type: "string", description: "The origin: ts | native | module (R-BRIDGE-008)." },
            { name: "tick", type: "generation", description: "The sim tick, when the entry is session-scoped." },
            { name: "session", type: "string", description: "The session id, when session-scoped." },
        ],
    },
};

/** Every topic name, in registry order. */
export const EVENT_TOPIC_NAMES: readonly EventTopic[] = ["files", "derivation", "diagnostics", "session", "clients", "log"];

/** The envelope every event frame carries (field names as the registry describes them). */
export const EVENT_ENVELOPE_FIELDS: readonly PayloadField[] = [
    { name: "seq", type: "generation", description: "The monotonic, totally-ordered event seq within the incarnation (R-BRIDGE-008)." },
    { name: "incarnationId", type: "string", description: "The daemon incarnation epoch; a restart forces a fresh snapshot rather than a stale since-seq resume." },
    { name: "generation", type: "generation", description: "The derived-world generation the event reflects." },
    { name: "topic", type: "string", description: "The event topic name." },
    { name: "payload", type: "json", description: "The topic-specific payload (see eventTopics)." },
];

/** Every error code in the uniform error catalog (R-CLI-007). */
export type ErrorCode =
    | "usage.invalid"
    | "usage.unknown_verb"
    | "usage.unknown_flag"
    | "usage.missing_argument"
    | "namespace.collision"
    | "file.not_found"
    | "file.parse_error"
    | "file.validation_failed"
    | "cas.mismatch"
    | "path.jail_violation"
    | "handshake.incompatible_protocol"
    | "version.mismatch"
    | "package.engine_incompatible"
    | "schema.newer_than_engine"
    | "schema.newer_than_package"
    | "contract.unimplemented"
    | "internal.error"
    | "scope.denied"
    | "merge.conflict"
    | "merge.id_conflict"
    | "merge.binary_sidecar"
    | "merge.meta_guid"
    | "merge.newer_stamped"
    | "merge.duplicate_id"
    | "merge.no_conflict_at_path"
    | "merge.rekey_target_invalid"
    | "scope.insufficient"
    | "contract.operational_only"
    | "resource.unknown_handle"
    | "migration.step_missing"
    | "migration.step_failed"
    | "migration.budget_exceeded"
    | "migration.id_mutated"
    | "migration.runner_unavailable"
    | "migration.orphan_override"
    | "sidecar.bad_magic"
    | "sidecar.truncated"
    | "sidecar.unsupported_version"
    | "sidecar.ref_malformed"
    | "sidecar.dangling_ref"
    | "sidecar.orphaned"
    | "sidecar.hash_mismatch"
    | "asset.guid_duplicate"
    | "asset.meta_orphaned"
    | "asset.meta_invalid"
    | "asset.heal_ambiguous"
    | "asset.ref_dangling"
    | "asset.ref_path_only"
    | "asset.ref_hint_stale"
    | "asset.move_source_missing"
    | "asset.move_destination_exists"
    | "asset.move_invalid"
    | "import.source_malformed"
    | "import.decode_failed"
    | "import.unsupported_format"
    | "import.jail_escape"
    | "import.subprocess_failed"
    | "import.non_deterministic"
    | "import.cache_corrupt"
    | "compose.write_target_not_found"
    | "compose.immutable_pointer"
    | "tilemap.chunk_oversize"
    | "tilemap.region_invalid"
    | "tilemap.id_duplicate"
    | "stringtable.locale_duplicate"
    | "stringtable.key_duplicate"
    | "stringtable.fallback_unknown"
    | "stringtable.fallback_cycle"
    | "stringtable.value_invalid"
    | "stringtable.plural_incomplete"
    | "stringtable.value_locale_duplicate"
    | "save.malformed"
    | "save.unknown_component"
    | "save.back_compat_exceeded"
    | "save.format_unsupported"
    | "session.state_invalid"
    | "session.state_not_found"
    | "session.input_invalid"
    | "replay.artifact_invalid"
    | "replay.manifest_drift"
    | "replay.divergence"
    | "query.syntax_error"
    | "query.unknown_operator"
    | "query.invalid_cursor"
    | "query.unsupported_surface"
    | "ts.transpile_failed"
    | "ts.bundle_failed"
    | "ts.type_error"
    | "ts.runtime_error"
    | "subscription.unknown_sub"
    | "install.version_unpinned"
    | "install.integrity_mismatch"
    | "install.lockfile_incomplete"
    | "install.scripts_required"
    | "install.fetch_failed"
    | "consent_required"
    | "debug.attach_failed"
    | "debug.unsupported"
    | "merge.invalid_stable_id"
    | "viewport.adapter_absent"
    | "viewport.surface_unavailable"
    | "viewport.render_failed"
    | "play.not_running"
    | "play.session_failed"
    | "play.step_failed"
    | "play.hot_reload_failed"
    | "determinism.attestation_fastmath_forbidden"
    | "determinism.attestation_strict_fp_missing"
    | "determinism.attestation_flags_unverified"
    | "physics3d.invalid_entity"
    | "physics3d.missing_component"
    | "physics3d.invalid_shape"
    | "physics3d.invalid_mass"
    | "physics3d.invalid_step"
    | "physics2d.invalid_entity"
    | "physics2d.missing_component"
    | "physics2d.invalid_shape"
    | "physics2d.invalid_mass"
    | "physics2d.invalid_step"
    | "anim.invalid_entity"
    | "anim.missing_component"
    | "anim.invalid_rig"
    | "anim.duplicate_component"
    | "anim.invalid_step"
    | "particle.invalid_entity"
    | "particle.missing_component"
    | "particle.invalid_config"
    | "particle.invalid_step"
    | "spline.invalid_entity"
    | "spline.missing_component"
    | "spline.invalid_path"
    | "spline.duplicate_component"
    | "spline.invalid_step"
    | "audio.invalid_entity"
    | "audio.invalid_bus"
    | "audio.invalid_event"
    | "audio.device_unavailable"
    | "input.invalid_context"
    | "input.duplicate_context"
    | "input.unknown_context"
    | "input.unknown_action"
    | "sim.gc.unavailable"
    | "sim.gc.invalid_budget"
    | "sim.gc.window_failed"
    | "net.invalid_net_id"
    | "net.duplicate_net_id"
    | "net.snapshot_component_mismatch"
    | "net.authority_conflict"
    | "ui.scene_not_found"
    | "ui.scene_invalid"
    | "ui.node_not_found"
    | "ui.invalid_event"
    | "ui.assertion_failed"
    | "transcode.no_target"
    | "transcode.unsupported_format"
    | "transcode.bad_descriptor"
    | "build.template_unverified"
    | "build.toolchain_fetch_failed"
    | "build.aot_failed"
    | "build.transcode_failed"
    | "build.link_failed"
    | "doctor.environment_incomplete"
    | "doctor.toolchain_missing"
    | "doctor.toolchain_version_mismatch"
    | "doctor.filesync_budget_low"
    | "doctor.signing_prereq_absent"
    | "doctor.unknown_target"
    | "build.artifact_unsigned"
    | "tilemap.layer_not_found"
    | "tilemap.cell_out_of_bounds"
    | "tilemap.tile_unknown"
    | "attach.denied"
    | "daemon.busy"
    | "editor.session_state_invalid";

/** A registry-projected description of one catalog error. */
export interface ErrorDescriptor {
    readonly code: ErrorCode;
    readonly message: string;
    /** Whether a client may retry the same request unchanged. */
    readonly retriable: boolean;
    readonly exitCode: number;
    /** The requirement id this error originates from. */
    readonly origin: string;
}

export const ERROR_CATALOG: { readonly [C in ErrorCode]: ErrorDescriptor } = {
    "usage.invalid": {
        code: "usage.invalid",
        message: "The command could not be parsed against the verb grammar.",
        retriable: false,
        exitCode: 2,
        origin: "R-CLI-007",
    },
    "usage.unknown_verb": {
        code: "usage.unknown_verb",
        message: "No such verb in the contract surface.",
        retriable: false,
        exitCode: 2,
        origin: "R-CLI-007",
    },
    "usage.unknown_flag": {
        code: "usage.unknown_flag",
        message: "Unknown flag for this verb.",
        retriable: false,
        exitCode: 2,
        origin: "R-CLI-007",
    },
    "usage.missing_argument": {
        code: "usage.missing_argument",
        message: "A required argument was not supplied.",
        retriable: false,
        exitCode: 2,
        origin: "R-CLI-007",
    },
    "namespace.collision": {
        code: "namespace.collision",
        message: "A package's reserved namespace collides with an already-registered one.",
        retriable: false,
        exitCode: 2,
        origin: "R-CLI-007",
    },
    "file.not_found": {
        code: "file.not_found",
        message: "The referenced file does not exist.",
        retriable: false,
        exitCode: 3,
        origin: "R-FILE-003",
    },
    "file.parse_error": {
        code: "file.parse_error",
        message: "The file is not well-formed and could not be parsed.",
        retriable: false,
        exitCode: 5,
        origin: "R-FILE-003",
    },
    "file.validation_failed": {
        code: "file.validation_failed",
        message: "The payload failed schema validation for its kind.",
        retriable: false,
        exitCode: 5,
        origin: "R-FILE-003",
    },
    "cas.mismatch": {
        code: "cas.mismatch",
        message: "The --if-match hash did not match the file's current bytes.",
        retriable: true,
        exitCode: 4,
        origin: "R-CLI-006",
    },
    "path.jail_violation": {
        code: "path.jail_violation",
        message: "The path escapes the project root and was refused.",
        retriable: false,
        exitCode: 6,
        origin: "R-SEC-008",
    },
    "handshake.incompatible_protocol": {
        code: "handshake.incompatible_protocol",
        message: "The client protocol major is outside the daemon's compatibility window.",
        retriable: false,
        exitCode: 7,
        origin: "R-CLI-010",
    },
    "version.mismatch": {
        code: "version.mismatch",
        message: "Engine/protocol versions are incompatible; attach refused.",
        retriable: false,
        exitCode: 7,
        origin: "R-BRIDGE-006",
    },
    "package.engine_incompatible": {
        code: "package.engine_incompatible",
        message: "The package declares an engine-compat range the running engine does not satisfy.",
        retriable: false,
        exitCode: 7,
        origin: "R-PKG-005",
    },
    "schema.newer_than_engine": {
        code: "schema.newer_than_engine",
        message: "The payload is stamped for a newer engine schema; last-good state retained.",
        retriable: false,
        exitCode: 5,
        origin: "R-PKG-005",
    },
    "schema.newer_than_package": {
        code: "schema.newer_than_package",
        message: "The payload is stamped for a newer package schema; last-good state retained.",
        retriable: false,
        exitCode: 5,
        origin: "R-PKG-005",
    },
    "contract.unimplemented": {
        code: "contract.unimplemented",
        message: "The verb surface is reserved in the contract but its backing is not wired in this version.",
        retriable: false,
        exitCode: 8,
        origin: "R-CLI-009",
    },
    "internal.error": {
        code: "internal.error",
        message: "An unexpected internal error occurred.",
        retriable: false,
        exitCode: 1,
        origin: "R-CLI-008",
    },
    "scope.denied": {
        code: "scope.denied",
        message: "The attach token's scope does not permit this method (least privilege, R-SEC-007).",
        retriable: false,
        exitCode: 6,
        origin: "R-SEC-007",
    },
    "merge.conflict": {
        code: "merge.conflict",
        message: "A structural three-way merge left an unresolved field conflict; see the conflict envelope.",
        retriable: false,
        exitCode: 4,
        origin: "R-FILE-012",
    },
    "merge.id_conflict": {
        code: "merge.id_conflict",
        message: "The same intra-file id was added on both sides relative to base \u2014 a structural conflict, never silently unified (L-33).",
        retriable: false,
        exitCode: 4,
        origin: "R-FILE-012",
    },
    "merge.binary_sidecar": {
        code: "merge.binary_sidecar",
        message: "A binary sidecar differs on both sides; binaries are never content-merged \u2014 resolve whole-file ours/theirs (L-33).",
        retriable: false,
        exitCode: 4,
        origin: "R-FILE-012",
    },
    "merge.meta_guid": {
        code: "merge.meta_guid",
        message: "Both sides minted a different GUID for the same asset meta; GUID identity is never field-blended \u2014 resolve whole-asset ours/theirs (L-36).",
        retriable: false,
        exitCode: 4,
        origin: "R-FILE-012",
    },
    "merge.newer_stamped": {
        code: "merge.newer_stamped",
        message: "A merge input carries payloads stamped newer than the installed schemas; a whole-file conflict class, never a parse error (L-37).",
        retriable: false,
        exitCode: 4,
        origin: "R-FILE-012",
    },
    "merge.duplicate_id": {
        code: "merge.duplicate_id",
        message: "Two objects in one file share an intra-file id (an id add/add or a raw copy); re-key one via `context re-key`. The post-merge convergence gate.",
        retriable: false,
        exitCode: 5,
        origin: "R-FILE-012",
    },
    "merge.no_conflict_at_path": {
        code: "merge.no_conflict_at_path",
        message: "`context resolve-conflict` named a --path with no open conflict in the merge sidecar.",
        retriable: false,
        exitCode: 3,
        origin: "R-FILE-012",
    },
    "merge.rekey_target_invalid": {
        code: "merge.rekey_target_invalid",
        message: "The re-key target does not resolve to an object carrying a stable intra-file id (or the requested id is invalid).",
        retriable: false,
        exitCode: 2,
        origin: "R-FILE-012",
    },
    "scope.insufficient": {
        code: "scope.insufficient",
        message: "The attach token holds a lower scope than this method requires; re-attach with the needed scope.",
        retriable: false,
        exitCode: 6,
        origin: "R-SEC-007",
    },
    "contract.operational_only": {
        code: "contract.operational_only",
        message: "This verb is operational: it is served by a live daemon over RPC (attach to a running 'context daemon'), not as a one-shot CLI verb.",
        retriable: false,
        exitCode: 2,
        origin: "R-CLI-009",
    },
    "resource.unknown_handle": {
        code: "resource.unknown_handle",
        message: "The resource handle is unknown to this daemon instance (expired, foreign, or malformed).",
        retriable: false,
        exitCode: 3,
        origin: "R-CLI-017",
    },
    "migration.step_missing": {
        code: "migration.step_missing",
        message: "No registered migration step covers a version in the payload's chain (a gap).",
        retriable: false,
        exitCode: 5,
        origin: "R-DATA-004",
    },
    "migration.step_failed": {
        code: "migration.step_failed",
        message: "A migration step reported failure; the document rolled back.",
        retriable: false,
        exitCode: 5,
        origin: "R-DATA-004",
    },
    "migration.budget_exceeded": {
        code: "migration.budget_exceeded",
        message: "A payload exceeded the per-invocation migration budget (L-37); the document rolled back.",
        retriable: false,
        exitCode: 5,
        origin: "R-DATA-004",
    },
    "migration.id_mutated": {
        code: "migration.id_mutated",
        message: "A migration step altered, moved, added, or removed an id/guid \u2014 identity is immutable (L-37); the document rolled back.",
        retriable: false,
        exitCode: 5,
        origin: "R-DATA-004",
    },
    "migration.runner_unavailable": {
        code: "migration.runner_unavailable",
        message: "Package-shipped migrations execute only in the sandboxed WASM tier; the VM component is not stood up yet (L-37 contract; execution deliberately stubbed).",
        retriable: false,
        exitCode: 8,
        origin: "R-DATA-004",
    },
    "migration.orphan_override": {
        code: "migration.orphan_override",
        message: "An override path has no destination after migration; the entry is preserved but excluded from flatten (L-37 orphan override).",
        retriable: false,
        exitCode: 5,
        origin: "R-DATA-004",
    },
    "sidecar.bad_magic": {
        code: "sidecar.bad_magic",
        message: "The file does not begin with the sidecar magic.",
        retriable: false,
        exitCode: 5,
        origin: "R-FILE-001",
    },
    "sidecar.truncated": {
        code: "sidecar.truncated",
        message: "The sidecar file is shorter than its fixed header.",
        retriable: false,
        exitCode: 5,
        origin: "R-FILE-001",
    },
    "sidecar.unsupported_version": {
        code: "sidecar.unsupported_version",
        message: "The sidecar header carries a format version this engine cannot read; last-good state retained.",
        retriable: false,
        exitCode: 5,
        origin: "R-FILE-001",
    },
    "sidecar.ref_malformed": {
        code: "sidecar.ref_malformed",
        message: "A $sidecar reference is not the canonical {\"$sidecar\": \"<relpath>\", \"hash\": \"<decimal>\"} shape.",
        retriable: false,
        exitCode: 5,
        origin: "R-FILE-003",
    },
    "sidecar.dangling_ref": {
        code: "sidecar.dangling_ref",
        message: "A $sidecar reference names a sidecar file that does not exist.",
        retriable: false,
        exitCode: 5,
        origin: "R-FILE-003",
    },
    "sidecar.orphaned": {
        code: "sidecar.orphaned",
        message: "A sidecar file exists with no referencing owner.",
        retriable: false,
        exitCode: 5,
        origin: "R-FILE-003",
    },
    "sidecar.hash_mismatch": {
        code: "sidecar.hash_mismatch",
        message: "The sidecar's whole-file raw bytes do not hash to the referencing \"hash\" value.",
        retriable: false,
        exitCode: 5,
        origin: "R-FILE-001",
    },
    "asset.guid_duplicate": {
        code: "asset.guid_duplicate",
        message: "Two live assets claim the same GUID (raw copy?); the lexicographically-first path keeps it \u2014 re-key the duplicate via `context validate --fix`.",
        retriable: false,
        exitCode: 5,
        origin: "R-ASSET-002",
    },
    "asset.meta_orphaned": {
        code: "asset.meta_orphaned",
        message: "A meta sidecar's asset file is missing (raw move or delete); healing pairs unique moves, `context validate --fix` cleans deliberate deletes.",
        retriable: false,
        exitCode: 5,
        origin: "R-ASSET-002",
    },
    "asset.meta_invalid": {
        code: "asset.meta_invalid",
        message: "A meta sidecar is malformed (not well-formed JSON, root not an object, or no valid \"guid\"); non-canonical formatting alone is tolerated.",
        retriable: false,
        exitCode: 5,
        origin: "R-ASSET-002",
    },
    "asset.heal_ambiguous": {
        code: "asset.heal_ambiguous",
        message: "Raw-move healing found no UNIQUE orphan/newcomer pairing; nothing was written \u2014 re-run the move via `context asset move` or resolve by hand.",
        retriable: false,
        exitCode: 5,
        origin: "R-ASSET-002",
    },
    "asset.ref_dangling": {
        code: "asset.ref_dangling",
        message: "A reference resolves to no indexed asset (unknown $ref GUID, or a path that names nothing).",
        retriable: false,
        exitCode: 5,
        origin: "R-ASSET-002",
    },
    "asset.ref_path_only": {
        code: "asset.ref_path_only",
        message: "A path-only reference (accepted, L-34); `context validate --fix` or the next tool save resolves the authoritative $ref GUID.",
        retriable: false,
        exitCode: 5,
        origin: "R-ASSET-002",
    },
    "asset.ref_hint_stale": {
        code: "asset.ref_hint_stale",
        message: "A dual-form reference's path hint no longer matches the asset's location; healed on tool save (L-34).",
        retriable: false,
        exitCode: 5,
        origin: "R-ASSET-002",
    },
    "asset.move_source_missing": {
        code: "asset.move_source_missing",
        message: "The move/rename source asset does not exist.",
        retriable: false,
        exitCode: 3,
        origin: "R-FILE-004",
    },
    "asset.move_destination_exists": {
        code: "asset.move_destination_exists",
        message: "The move/rename destination is occupied by a different asset (or an orphaned sidecar holding one's identity); move never overwrites.",
        retriable: false,
        exitCode: 4,
        origin: "R-FILE-004",
    },
    "asset.move_invalid": {
        code: "asset.move_invalid",
        message: "The move/rename request is malformed (a sidecar, temp-residue, or dot-tree path, or an empty path).",
        retriable: false,
        exitCode: 2,
        origin: "R-FILE-004",
    },
    "import.source_malformed": {
        code: "import.source_malformed",
        message: "The source asset is not the format its extension claims, or its container is truncated / structurally invalid; nothing was imported.",
        retriable: false,
        exitCode: 5,
        origin: "R-ASSET-001",
    },
    "import.decode_failed": {
        code: "import.decode_failed",
        message: "The source asset's container parsed but its contents could not be decoded (e.g. a chunk CRC mismatch or an unreadable payload).",
        retriable: false,
        exitCode: 5,
        origin: "R-ASSET-001",
    },
    "import.unsupported_format": {
        code: "import.unsupported_format",
        message: "The source asset uses a format variant this importer does not support in v1 (e.g. a non-PCM WAV or a non-2.0 glTF); it is refused, never silently mis-imported.",
        retriable: false,
        exitCode: 5,
        origin: "R-ASSET-001",
    },
    "import.jail_escape": {
        code: "import.jail_escape",
        message: "An importer run attempted to read or write outside its TOCTOU-safe path jail, or requested the denied network capability; the run was refused (R-SEC-006/008/010).",
        retriable: false,
        exitCode: 6,
        origin: "R-SEC-008",
    },
    "import.subprocess_failed": {
        code: "import.subprocess_failed",
        message: "The isolated importer subprocess failed to spawn, was killed by its per-OS sandbox primitive (seccomp-bpf and friends), or exited without returning a result; nothing was imported and the run fails closed (R-SEC-006).",
        retriable: false,
        exitCode: 1,
        origin: "R-SEC-006",
    },
    "import.non_deterministic": {
        code: "import.non_deterministic",
        message: "An importer produced different bytes across the CI double-run byte-compare; the shared cache is only sound for run-deterministic importers, so this fails the gate.",
        retriable: false,
        exitCode: 1,
        origin: "R-ASSET-001",
    },
    "import.cache_corrupt": {
        code: "import.cache_corrupt",
        message: "A shared-cache entry failed its content-hash self-verification on read (corruption); the entry is rejected and the artifact re-derived (R-FILE-010).",
        retriable: true,
        exitCode: 5,
        origin: "R-FILE-010",
    },
    "compose.write_target_not_found": {
        code: "compose.write_target_not_found",
        message: "The composed-write target does not resolve \u2014 the id-path names no composed entity, or an --at-instance prefix names no instancing scene.",
        retriable: false,
        exitCode: 3,
        origin: "R-CLI-006",
    },
    "compose.immutable_pointer": {
        code: "compose.immutable_pointer",
        message: "The field pointer addresses an identity field (/id, /$schema, /version) that is immutable under composition (L-37).",
        retriable: false,
        exitCode: 5,
        origin: "R-CLI-006",
    },
    "tilemap.chunk_oversize": {
        code: "tilemap.chunk_oversize",
        message: "A tilemap chunk's packed cell payload exceeds the ~1 MB split-nudge ceiling; split the region (L-33 advisory).",
        retriable: false,
        exitCode: 5,
        origin: "R-2D-003",
    },
    "tilemap.region_invalid": {
        code: "tilemap.region_invalid",
        message: "A tilemap chunk region has a non-positive width or height.",
        retriable: false,
        exitCode: 5,
        origin: "R-2D-003",
    },
    "tilemap.id_duplicate": {
        code: "tilemap.id_duplicate",
        message: "Two tilemap tile-sets or two layers share a stable id (L-33 ids are unique within a collection).",
        retriable: false,
        exitCode: 5,
        origin: "R-2D-003",
    },
    "stringtable.locale_duplicate": {
        code: "stringtable.locale_duplicate",
        message: "Two string-table locales declare the same tag.",
        retriable: false,
        exitCode: 5,
        origin: "R-I18N-001",
    },
    "stringtable.key_duplicate": {
        code: "stringtable.key_duplicate",
        message: "Two string-table entries declare the same key.",
        retriable: false,
        exitCode: 5,
        origin: "R-I18N-001",
    },
    "stringtable.fallback_unknown": {
        code: "stringtable.fallback_unknown",
        message: "A locale's fallback names a locale not declared in the table's `locales`.",
        retriable: false,
        exitCode: 5,
        origin: "R-I18N-001",
    },
    "stringtable.fallback_cycle": {
        code: "stringtable.fallback_cycle",
        message: "A locale's fallback chain contains a cycle.",
        retriable: false,
        exitCode: 5,
        origin: "R-I18N-001",
    },
    "stringtable.value_invalid": {
        code: "stringtable.value_invalid",
        message: "A string-table translation is not EXACTLY ONE of `text` or `plural`.",
        retriable: false,
        exitCode: 5,
        origin: "R-I18N-001",
    },
    "stringtable.plural_incomplete": {
        code: "stringtable.plural_incomplete",
        message: "A plural set omits the required CLDR `other` category.",
        retriable: false,
        exitCode: 5,
        origin: "R-I18N-001",
    },
    "stringtable.value_locale_duplicate": {
        code: "stringtable.value_locale_duplicate",
        message: "Two translations for one string-table key declare the same locale.",
        retriable: false,
        exitCode: 5,
        origin: "R-I18N-001",
    },
    "save.malformed": {
        code: "save.malformed",
        message: "The save document shape is invalid (not a save envelope, a bad composed identity, or a component payload the save header does not stamp).",
        retriable: false,
        exitCode: 5,
        origin: "R-DATA-005",
    },
    "save.unknown_component": {
        code: "save.unknown_component",
        message: "The save carries a component this build's compiled component set does not include; a save migration runner embeds migrations for exactly the compiled set (R-DATA-005).",
        retriable: false,
        exitCode: 5,
        origin: "R-DATA-005",
    },
    "save.back_compat_exceeded": {
        code: "save.back_compat_exceeded",
        message: "A saved component payload is stamped more schema versions behind than the declared save back-compat scope (N versions) covers; refused, never best-effort read (R-DATA-005).",
        retriable: false,
        exitCode: 5,
        origin: "R-DATA-005",
    },
    "save.format_unsupported": {
        code: "save.format_unsupported",
        message: "The save envelope's format version is newer than this build reads; last-good retained, never a best-effort parse (R-DATA-005).",
        retriable: false,
        exitCode: 5,
        origin: "R-DATA-005",
    },
    "session.state_invalid": {
        code: "session.state_invalid",
        message: "The session-state document is malformed, an unsupported version, or names ids that are not restorable.",
        retriable: false,
        exitCode: 5,
        origin: "R-QA-005",
    },
    "session.state_not_found": {
        code: "session.state_not_found",
        message: "The named session-state file does not exist.",
        retriable: false,
        exitCode: 3,
        origin: "R-QA-005",
    },
    "session.input_invalid": {
        code: "session.input_invalid",
        message: "A synthetic input event / action activation injection was malformed (unknown kind or a missing field).",
        retriable: false,
        exitCode: 2,
        origin: "R-QA-005",
    },
    "replay.artifact_invalid": {
        code: "replay.artifact_invalid",
        message: "The replay artifact is malformed or an unsupported version.",
        retriable: false,
        exitCode: 5,
        origin: "R-QA-005",
    },
    "replay.manifest_drift": {
        code: "replay.manifest_drift",
        message: "The project inputs drifted from the replay artifact's content manifest; reported as drift BEFORE running, never a silent divergence.",
        retriable: false,
        exitCode: 4,
        origin: "R-QA-005",
    },
    "replay.divergence": {
        code: "replay.divergence",
        message: "A deterministic replay diverged from its expected per-tick hash trace; the first divergent tick is reported.",
        retriable: false,
        exitCode: 5,
        origin: "R-QA-005",
    },
    "query.syntax_error": {
        code: "query.syntax_error",
        message: "The query expression could not be parsed against the R-CLI-012 grammar; see the byte offset in the diagnostic pointer.",
        retriable: false,
        exitCode: 2,
        origin: "R-CLI-012",
    },
    "query.unknown_operator": {
        code: "query.unknown_operator",
        message: "The query used an operator or predicate function not in the enumerated R-CLI-012 operator set (equality / range / existence / string-match).",
        retriable: false,
        exitCode: 2,
        origin: "R-CLI-012",
    },
    "query.invalid_cursor": {
        code: "query.invalid_cursor",
        message: "The pagination cursor is malformed, from a foreign daemon incarnation, or not the unified R-CLI-012 / R-BRIDGE-008 cursor shape; re-issue the query without a cursor.",
        retriable: false,
        exitCode: 2,
        origin: "R-CLI-012",
    },
    "query.unsupported_surface": {
        code: "query.unsupported_surface",
        message: "The query named a surface other than the derived world, live-sim state, or schema introspection \u2014 the one language spans exactly those three.",
        retriable: false,
        exitCode: 2,
        origin: "R-CLI-012",
    },
    "ts.transpile_failed": {
        code: "ts.transpile_failed",
        message: "TypeScript could not be transpiled to JavaScript (a syntax or transform error); see the diagnostic text.",
        retriable: false,
        exitCode: 5,
        origin: "R-LANG-002",
    },
    "ts.bundle_failed": {
        code: "ts.bundle_failed",
        message: "The TypeScript entrypoint could not be bundled (an unresolved import or transform error); see the diagnostic text.",
        retriable: false,
        exitCode: 5,
        origin: "R-LANG-002",
    },
    "ts.type_error": {
        code: "ts.type_error",
        message: "TypeScript failed a semantic typecheck (a --noEmit type error); the diagnostic carries the tsc code (TSxxxx) and the authored .ts line:column.",
        retriable: false,
        exitCode: 5,
        origin: "R-LANG-002",
    },
    "ts.runtime_error": {
        code: "ts.runtime_error",
        message: "Authored TypeScript threw at runtime; the diagnostic carries a TS-source-mapped stack trace (authored .ts position, not the transpiled JS position).",
        retriable: false,
        exitCode: 5,
        origin: "R-OBS-005",
    },
    "subscription.unknown_sub": {
        code: "subscription.unknown_sub",
        message: "The subscription id is not a live subscription on this daemon incarnation (never subscribed, already unsubscribed, or from a prior incarnation); re-subscribe.",
        retriable: false,
        exitCode: 3,
        origin: "R-CLI-015",
    },
    "install.version_unpinned": {
        code: "install.version_unpinned",
        message: "An engine-driven install requires an exact pinned version; a dependency spec is a range, dist-tag, or url (R-SEC-005). The install is refused, never floated.",
        retriable: false,
        exitCode: 5,
        origin: "R-SEC-005",
    },
    "install.integrity_mismatch": {
        code: "install.integrity_mismatch",
        message: "A fetched package artifact's bytes did not match its lockfile integrity (SRI) hash, or the SRI named no verifiable algorithm; the artifact is refused, never used with a warning (verify-before-use, fail-closed \u2014 R-SEC-009).",
        retriable: false,
        exitCode: 5,
        origin: "R-SEC-005",
    },
    "install.lockfile_incomplete": {
        code: "install.lockfile_incomplete",
        message: "A dependency is missing from the lockfile, or a lock entry lacks an exact version / integrity \u2014 the dependency graph is not fully pinned (incl. transitive), so the install is refused fail-closed.",
        retriable: false,
        exitCode: 5,
        origin: "R-SEC-005",
    },
    "install.scripts_required": {
        code: "install.scripts_required",
        message: "The package declares an install lifecycle script, classifying it native-tier; engine-driven installs never run lifecycle scripts (--ignore-scripts, all tiers), so it is refused pending the L-49 consent gate (see consent_required).",
        retriable: false,
        exitCode: 6,
        origin: "R-SEC-005",
    },
    "install.fetch_failed": {
        code: "install.fetch_failed",
        message: "The package source could not supply a pinned artifact's bytes (e.g. the offline --source cache lacks the tarball) \u2014 a source/cache miss, distinct from a lockfile-completeness or integrity defect. The install is refused fail-closed.",
        retriable: false,
        exitCode: 5,
        origin: "R-SEC-005",
    },
    "consent_required": {
        code: "consent_required",
        message: "A native-tier / build+install action was requested without the needed grant; it returns this machine-readable code (carrying the requested scope + an approval ref) and resumes the same idempotency-keyed op once granted out-of-band (R-SEC-011).",
        retriable: false,
        exitCode: 6,
        origin: "R-SEC-011",
    },
    "debug.attach_failed": {
        code: "debug.attach_failed",
        message: "Attaching the V8 in-box CDP inspector session failed (the inspector could not be created or connected through EditorKernel); no debug session was established.",
        retriable: false,
        exitCode: 1,
        origin: "R-OBS-005",
    },
    "debug.unsupported": {
        code: "debug.unsupported",
        message: "This build has no V8 backend, so no CDP inspector can be attached; the debug attach is available only where the V8 host is linked (the CI/MSVC-tier builds).",
        retriable: false,
        exitCode: 8,
        origin: "R-OBS-005",
    },
    "merge.invalid_stable_id": {
        code: "merge.invalid_stable_id",
        message: "A stable intra-file id is not the L-33 form (16..32 lowercase hex chars); re-key it via `context re-key`. The `context validate` FORMAT gate, sibling to merge.duplicate_id.",
        retriable: false,
        exitCode: 5,
        origin: "R-FILE-012",
    },
    "viewport.adapter_absent": {
        code: "viewport.adapter_absent",
        message: "No GPU adapter is available to render the observer viewport; absence is reported, never a fabricated frame (R-HEAD-002).",
        retriable: false,
        exitCode: 8,
        origin: "R-HEAD-002",
    },
    "viewport.surface_unavailable": {
        code: "viewport.surface_unavailable",
        message: "The L-41 CEF compositing surface for the observer viewport could not be acquired (e.g. the selected mode needs a GPU shared-texture surface but no GPU compositor is present).",
        retriable: false,
        exitCode: 1,
        origin: "R-UI-007",
    },
    "viewport.render_failed": {
        code: "viewport.render_failed",
        message: "The observer viewport's scene render or pixel readback failed (R-REND-002).",
        retriable: false,
        exitCode: 1,
        origin: "R-REND-002",
    },
    "play.not_running": {
        code: "play.not_running",
        message: "A play control (pause / step / hot-reload) was issued with no live play session; start play first (L-51 edit state).",
        retriable: false,
        exitCode: 2,
        origin: "R-PLAY-001",
    },
    "play.session_failed": {
        code: "play.session_failed",
        message: "The play session could not be started over the edit state; nothing was played and no authored file was written (L-51 fail-closed).",
        retriable: false,
        exitCode: 1,
        origin: "R-PLAY-001",
    },
    "play.step_failed": {
        code: "play.step_failed",
        message: "Advancing the running play session by a fixed tick failed; the session state is unchanged (R-SIM-002 fail-closed).",
        retriable: false,
        exitCode: 1,
        origin: "R-PLAY-002",
    },
    "play.hot_reload_failed": {
        code: "play.hot_reload_failed",
        message: "A live authored edit could not be reflected into the running play session (L-22 hot reload); the running session is unchanged.",
        retriable: false,
        exitCode: 5,
        origin: "R-PLAY-003",
    },
    "determinism.attestation_fastmath_forbidden": {
        code: "determinism.attestation_fastmath_forbidden",
        message: "A forbidden relaxed-FP flag (fast-math) reached the sim path of a deterministic build; deterministic:true is refused, never forged (R-SIM-005).",
        retriable: false,
        exitCode: 5,
        origin: "R-SIM-005",
    },
    "determinism.attestation_strict_fp_missing": {
        code: "determinism.attestation_strict_fp_missing",
        message: "A deterministic build did not have the strict floating-point model in effect (MSVC /fp:strict); the produced attestation fails closed rather than claim unverified determinism.",
        retriable: false,
        exitCode: 5,
        origin: "R-SIM-005",
    },
    "determinism.attestation_flags_unverified": {
        code: "determinism.attestation_flags_unverified",
        message: "A deterministic build was requested but recorded no applied strict-FP flags to attest over; the attestation cannot be PRODUCED from verified flags, so it is refused (R-SEC-009 fail-closed).",
        retriable: false,
        exitCode: 5,
        origin: "R-SIM-005",
    },
    "physics3d.invalid_entity": {
        code: "physics3d.invalid_entity",
        message: "A dead or null entity handle was passed to a physics operation; nothing was simulated or modified (fail-closed).",
        retriable: false,
        exitCode: 2,
        origin: "R-SYS-001",
    },
    "physics3d.missing_component": {
        code: "physics3d.missing_component",
        message: "A physics operation targeted an entity that lacks the full physics component set (transform + velocity + body + collider); the world is unchanged.",
        retriable: false,
        exitCode: 2,
        origin: "R-SYS-001",
    },
    "physics3d.invalid_shape": {
        code: "physics3d.invalid_shape",
        message: "A collider was rejected: a sphere radius or box half-extent was not positive; no physics components were added (fail-closed validation).",
        retriable: false,
        exitCode: 5,
        origin: "R-SYS-001",
    },
    "physics3d.invalid_mass": {
        code: "physics3d.invalid_mass",
        message: "A dynamic body was rejected: its mass was not positive; no physics components were added (fail-closed validation).",
        retriable: false,
        exitCode: 5,
        origin: "R-SYS-001",
    },
    "physics3d.invalid_step": {
        code: "physics3d.invalid_step",
        message: "A physics simulation step was refused: the fixed tick duration was not positive; the world is unchanged.",
        retriable: false,
        exitCode: 5,
        origin: "R-SYS-001",
    },
    "physics2d.invalid_entity": {
        code: "physics2d.invalid_entity",
        message: "A dead or null entity handle was passed to a physics operation; nothing was simulated or modified (fail-closed).",
        retriable: false,
        exitCode: 2,
        origin: "R-2D-002",
    },
    "physics2d.missing_component": {
        code: "physics2d.missing_component",
        message: "A physics operation targeted an entity that lacks the full physics component set (transform + velocity + body + collider); the world is unchanged.",
        retriable: false,
        exitCode: 2,
        origin: "R-2D-002",
    },
    "physics2d.invalid_shape": {
        code: "physics2d.invalid_shape",
        message: "A collider was rejected: a circle radius or box half-extent was not positive; no physics components were added (fail-closed validation).",
        retriable: false,
        exitCode: 5,
        origin: "R-2D-002",
    },
    "physics2d.invalid_mass": {
        code: "physics2d.invalid_mass",
        message: "A dynamic body was rejected: its mass was not positive; no physics components were added (fail-closed validation).",
        retriable: false,
        exitCode: 5,
        origin: "R-2D-002",
    },
    "physics2d.invalid_step": {
        code: "physics2d.invalid_step",
        message: "A physics simulation step was refused: the fixed tick duration was not positive; the world is unchanged.",
        retriable: false,
        exitCode: 5,
        origin: "R-2D-002",
    },
    "anim.invalid_entity": {
        code: "anim.invalid_entity",
        message: "A dead or null entity handle was passed to an animation operation; nothing was simulated or modified (fail-closed).",
        retriable: false,
        exitCode: 2,
        origin: "R-SYS-002",
    },
    "anim.missing_component": {
        code: "anim.missing_component",
        message: "An animation operation targeted an entity that lacks the animator component; the world is unchanged.",
        retriable: false,
        exitCode: 2,
        origin: "R-SYS-002",
    },
    "anim.invalid_rig": {
        code: "anim.invalid_rig",
        message: "A rig was rejected: it has no clips or graph states, an out-of-range initial state, a non-positive clip duration, or a graph state / transition naming a non-existent clip or state; the AnimationWorld keeps its previous rig (fail-closed validation).",
        retriable: false,
        exitCode: 5,
        origin: "R-SYS-008",
    },
    "anim.duplicate_component": {
        code: "anim.duplicate_component",
        message: "An animator could not be attached: the entity already carries one; nothing was overwritten (fail-closed validation).",
        retriable: false,
        exitCode: 5,
        origin: "R-SYS-002",
    },
    "anim.invalid_step": {
        code: "anim.invalid_step",
        message: "An animation simulation step was refused: the fixed tick duration was not positive; the world is unchanged.",
        retriable: false,
        exitCode: 5,
        origin: "R-SYS-002",
    },
    "particle.invalid_entity": {
        code: "particle.invalid_entity",
        message: "A dead or null entity handle was passed to a particle operation; nothing was simulated or modified (fail-closed).",
        retriable: false,
        exitCode: 2,
        origin: "R-SYS-003",
    },
    "particle.missing_component": {
        code: "particle.missing_component",
        message: "A particle operation targeted an entity that lacks the emitter component; the world is unchanged.",
        retriable: false,
        exitCode: 2,
        origin: "R-SYS-003",
    },
    "particle.invalid_config": {
        code: "particle.invalid_config",
        message: "An emitter description was rejected: a negative emission rate, a non-positive particle lifetime, or a negative velocity spread; no component was added (fail-closed validation).",
        retriable: false,
        exitCode: 5,
        origin: "R-SYS-003",
    },
    "particle.invalid_step": {
        code: "particle.invalid_step",
        message: "A particle simulation step was refused: the fixed tick duration was not positive; the world is unchanged.",
        retriable: false,
        exitCode: 5,
        origin: "R-SYS-003",
    },
    "spline.invalid_entity": {
        code: "spline.invalid_entity",
        message: "A dead or null entity handle was passed to a spline operation; nothing was simulated or modified (fail-closed).",
        retriable: false,
        exitCode: 2,
        origin: "R-SYS-004",
    },
    "spline.missing_component": {
        code: "spline.missing_component",
        message: "A spline operation targeted an entity that lacks the path-follower component; the world is unchanged.",
        retriable: false,
        exitCode: 2,
        origin: "R-SYS-004",
    },
    "spline.invalid_path": {
        code: "spline.invalid_path",
        message: "A path selection was rejected: the installed curve set is empty or malformed, or a follower named an out-of-range path index; no follower is attached and the world keeps its prior paths (fail-closed validation).",
        retriable: false,
        exitCode: 5,
        origin: "R-SYS-004",
    },
    "spline.duplicate_component": {
        code: "spline.duplicate_component",
        message: "A path follower could not be attached: the entity already carries one; nothing was overwritten (fail-closed validation).",
        retriable: false,
        exitCode: 5,
        origin: "R-SYS-004",
    },
    "spline.invalid_step": {
        code: "spline.invalid_step",
        message: "A spline simulation step was refused: the fixed tick duration was not positive; the world is unchanged.",
        retriable: false,
        exitCode: 5,
        origin: "R-SYS-004",
    },
    "audio.invalid_entity": {
        code: "audio.invalid_entity",
        message: "A dead or null entity handle was passed to an audio observe/spatialize operation; nothing was read or triggered (fail-closed).",
        retriable: false,
        exitCode: 2,
        origin: "R-SYS-006",
    },
    "audio.invalid_bus": {
        code: "audio.invalid_bus",
        message: "An audio mixing-bus graph was rejected: it is empty, has a duplicate bus id, or a bus names a non-existent or cyclic parent; no bus graph is installed (fail-closed validation).",
        retriable: false,
        exitCode: 5,
        origin: "R-SYS-006",
    },
    "audio.invalid_event": {
        code: "audio.invalid_event",
        message: "An audio event was rejected: a negative gain, an inverted/degenerate spatialization range, or a reference to an out-of-range bus; nothing was triggered (fail-closed validation).",
        retriable: false,
        exitCode: 5,
        origin: "R-SYS-006",
    },
    "audio.device_unavailable": {
        code: "audio.device_unavailable",
        message: "The audio device could not be initialized; audio playback is disabled. The simulation is unaffected \u2014 audio is a presentation observer off the sim path (fail-closed for audio only).",
        retriable: false,
        exitCode: 1,
        origin: "R-SYS-006",
    },
    "input.invalid_context": {
        code: "input.invalid_context",
        message: "An input context was rejected: an empty context id, or a binding with an empty device/code/action or an unrecognized device source; no context was installed (fail-closed validation).",
        retriable: false,
        exitCode: 5,
        origin: "R-SYS-007",
    },
    "input.duplicate_context": {
        code: "input.duplicate_context",
        message: "An input context could not be installed: its id is already installed; nothing was overwritten (fail-closed validation).",
        retriable: false,
        exitCode: 5,
        origin: "R-SYS-007",
    },
    "input.unknown_context": {
        code: "input.unknown_context",
        message: "An input operation named a context id that is not installed (or popped an empty active stack); the active stack is unchanged.",
        retriable: false,
        exitCode: 2,
        origin: "R-SYS-007",
    },
    "input.unknown_action": {
        code: "input.unknown_action",
        message: "A rebind named an action that has no binding in the target context; nothing was repointed (fail-closed).",
        retriable: false,
        exitCode: 2,
        origin: "R-SYS-007",
    },
    "sim.gc.unavailable": {
        code: "sim.gc.unavailable",
        message: "A JS-tier GC-discipline or GC-profiler operation needs the in-process JS VM, but this build carries the stub backend; nothing ran (fail-closed \u2014 the simulation is unaffected).",
        retriable: false,
        exitCode: 1,
        origin: "R-SIM-008",
    },
    "sim.gc.invalid_budget": {
        code: "sim.gc.invalid_budget",
        message: "A scheduled inter-tick GC window was refused: the requested pause budget is not a finite positive duration; nothing was collected (fail-closed validation).",
        retriable: false,
        exitCode: 5,
        origin: "R-SIM-008",
    },
    "sim.gc.window_failed": {
        code: "sim.gc.window_failed",
        message: "The JS VM reported a failure while running a scheduled inter-tick GC window or a GC-profiler query; the simulation state is unaffected (GC touches the JS heap only).",
        retriable: false,
        exitCode: 1,
        origin: "R-SIM-008",
    },
    "net.invalid_net_id": {
        code: "net.invalid_net_id",
        message: "A replication registration used the unassigned network identity (net id 0); nothing was registered (fail-closed validation).",
        retriable: false,
        exitCode: 5,
        origin: "R-NET-001",
    },
    "net.duplicate_net_id": {
        code: "net.duplicate_net_id",
        message: "Two entities were registered for replication with the same network identity; the second was refused so the composed-id keyed mapping stays 1:1 (fail-closed validation).",
        retriable: false,
        exitCode: 5,
        origin: "R-NET-001",
    },
    "net.snapshot_component_mismatch": {
        code: "net.snapshot_component_mismatch",
        message: "A state-sync snapshot carried a component payload whose byte length disagrees with the replicated component set's declared size for that component; nothing was applied and the replica is unchanged (fail-closed validation).",
        retriable: false,
        exitCode: 5,
        origin: "R-NET-001",
    },
    "net.authority_conflict": {
        code: "net.authority_conflict",
        message: "A state-sync delta targeted an entity the replica itself holds authority over; the authoritative peer's local state wins, so the delta is refused and the replica is unchanged.",
        retriable: false,
        exitCode: 2,
        origin: "R-NET-001",
    },
    "ui.scene_not_found": {
        code: "ui.scene_not_found",
        message: "The named UI-scene file does not exist.",
        retriable: false,
        exitCode: 3,
        origin: "R-UI-006",
    },
    "ui.scene_invalid": {
        code: "ui.scene_invalid",
        message: "The UI-scene document is malformed, an unsupported version, or names an unknown role or event kind; nothing was built (fail-closed validation).",
        retriable: false,
        exitCode: 5,
        origin: "R-UI-006",
    },
    "ui.node_not_found": {
        code: "ui.node_not_found",
        message: "A UI drive/assert verb named a node (by author name) that is not present in the tree.",
        retriable: false,
        exitCode: 3,
        origin: "R-UI-006",
    },
    "ui.invalid_event": {
        code: "ui.invalid_event",
        message: "A `ui send` request was malformed: an unknown event kind, or a required field for it (target node, key code, or text) was missing; nothing was dispatched (fail-closed).",
        retriable: false,
        exitCode: 2,
        origin: "R-UI-006",
    },
    "ui.assertion_failed": {
        code: "ui.assertion_failed",
        message: "A `ui assert` expectation did not hold over the loaded tree; the diagnostic reports the asserted fact, the expected value, and the actual value (the headless-CLI assert verdict).",
        retriable: false,
        exitCode: 5,
        origin: "R-UI-006",
    },
    "transcode.no_target": {
        code: "transcode.no_target",
        message: "The per-platform transcode table has no row for this (artifact kind, platform); the variant is surfaced as unavailable, never guessed (R-BUILD-003).",
        retriable: false,
        exitCode: 5,
        origin: "R-BUILD-003",
    },
    "transcode.unsupported_format": {
        code: "transcode.unsupported_format",
        message: "The transcode table named a target engine format this encoder cannot produce (e.g. an uncompressed row); a table gap, not a malformed source asset.",
        retriable: false,
        exitCode: 5,
        origin: "R-BUILD-003",
    },
    "transcode.bad_descriptor": {
        code: "transcode.bad_descriptor",
        message: "A described-kind artifact could not be transcoded because its bytes are not a well-formed descriptor; nothing was produced (fail-closed).",
        retriable: false,
        exitCode: 5,
        origin: "R-BUILD-003",
    },
    "build.template_unverified": {
        code: "build.template_unverified",
        message: "The runnable project/template failed pre-build verification (a missing, malformed, or empty root scene, or a blocking composition diagnostic); OR the R-BUILD-004 export template (the shipped --runtime host binary) failed verify-before-use against the pinned trust root (R-SEC-009 / L-58 \u2014 a tampered, unsigned, or untrusted-key signature). Nothing was built/packaged (fail-closed validation).",
        retriable: false,
        exitCode: 5,
        origin: "R-BUILD-002",
    },
    "build.toolchain_fetch_failed": {
        code: "build.toolchain_fetch_failed",
        message: "The per-target toolchain manifest (R-PKG-002 / L-42) could not supply the requested target's toolchain (no manifest entry for the target); OR an engine-fetched toolchain artifact failed verify-before-use against the pinned trust root (R-SEC-009 \u2014 an unverifiable fetch is a failed fetch). The toolchain cannot be fetched/trusted. Transient \u2014 a re-fetch against a repaired manifest or a correctly-signed artifact can succeed.",
        retriable: true,
        exitCode: 1,
        origin: "R-PKG-002",
    },
    "build.aot_failed": {
        code: "build.aot_failed",
        message: "The authored-script (TypeScript) AOT tier could not be produced for the target \u2014 a malformed or unresolvable script entrypoint; the build is refused fail-closed (deterministic).",
        retriable: false,
        exitCode: 1,
        origin: "R-BUILD-002",
    },
    "build.transcode_failed": {
        code: "build.transcode_failed",
        message: "A per-platform asset transcode node failed while producing the target's variant (the diagnostic carries the a03 transcode.* detail code); nothing was packed (fail-closed, deterministic).",
        retriable: false,
        exitCode: 1,
        origin: "R-BUILD-003",
    },
    "build.link_failed": {
        code: "build.link_failed",
        message: "The final-link path failed: the R-KERNEL-003 generated-registration TU references a package with no registrable module (an undefined register_<pkg> \u2014 the link's undefined-symbol failure). Deterministic \u2014 a bare retry cannot conjure the missing module.",
        retriable: false,
        exitCode: 1,
        origin: "R-KERNEL-003",
    },
    "doctor.environment_incomplete": {
        code: "doctor.environment_incomplete",
        message: "One or more required toolchain components for a requested target are missing or the wrong version; `context doctor` refuses (the environment cannot build the requested target(s)). The report enumerates each finding with its fetchable-vs-preinstalled remediation (fail-closed).",
        retriable: false,
        exitCode: 5,
        origin: "R-BUILD-008",
    },
    "doctor.toolchain_missing": {
        code: "doctor.toolchain_missing",
        message: "A required toolchain component is absent for a requested target; the finding's `fetchable` flag says whether the fix is an engine-fetch (via the a08-verified path, R-SEC-009) or a dev-preinstalled prerequisite. Deterministic \u2014 a bare re-run cannot conjure the component.",
        retriable: false,
        exitCode: 5,
        origin: "R-BUILD-008",
    },
    "doctor.toolchain_version_mismatch": {
        code: "doctor.toolchain_version_mismatch",
        message: "A required toolchain component is present but its version does not satisfy the L-42 manifest pin; blocking only when the pin's enforcement is `strict` (an advisory/documented drift is a non-blocking warning \u2014 the drift-alarm, not a build blocker).",
        retriable: false,
        exitCode: 5,
        origin: "R-BUILD-008",
    },
    "doctor.filesync_budget_low": {
        code: "doctor.filesync_budget_low",
        message: "The per-user file-sync watch budget is below the project file count \u00d7 worktree-daemon count (the up-front R-FILE-002 check). ADVISORY: raise the limit, or expect the watcher.degraded background-crawl fallback. Pairs with the R-FILE-011 N-daemons-on-one-box scenario.",
        retriable: false,
        exitCode: 5,
        origin: "R-FILE-002",
    },
    "doctor.signing_prereq_absent": {
        code: "doctor.signing_prereq_absent",
        message: "A code-signing prerequisite for a requested target (Windows Authenticode identity, macOS signing identity + notary creds) is not configured/reachable. ADVISORY (a ship-time prereq, never a build blocker). Presence only \u2014 the doctor never surfaces a secret/key value.",
        retriable: false,
        exitCode: 5,
        origin: "R-BUILD-005",
    },
    "doctor.unknown_target": {
        code: "doctor.unknown_target",
        message: "`context doctor --target <t>` named a value that is not a known build target (windows | linux | macos | web).",
        retriable: false,
        exitCode: 2,
        origin: "R-BUILD-008",
    },
    "build.artifact_unsigned": {
        code: "build.artifact_unsigned",
        message: "A produced artifact for a target that REQUIRES code-signing (Windows Authenticode, R-SEC-003) carries no signature \u2014 an advisory, never-silent WARNING folded into the build SUCCESS envelope's data.signing (e.g. a fork PR with no signing secrets). The build still succeeded; the artifact must be Authenticode-signed (Azure Trusted Signing, or the developer-certificate fallback, with a mandatory RFC-3161 timestamp) before shipping. Deterministic.",
        retriable: false,
        exitCode: 5,
        origin: "R-SEC-003",
    },
    "tilemap.layer_not_found": {
        code: "tilemap.layer_not_found",
        message: "The addressed tilemap layer id exists in no `layers` entry of the document (layers are addressed by their L-33 stable id, never by index or name).",
        retriable: false,
        exitCode: 3,
        origin: "R-2D-003",
    },
    "tilemap.cell_out_of_bounds": {
        code: "tilemap.cell_out_of_bounds",
        message: "A cell edit lies inside no chunk `region` of the addressed layer \u2014 v1 authoring rewrites EXISTING chunks and never invents new ones (the chunk topology is the M2 kind's).",
        retriable: false,
        exitCode: 5,
        origin: "R-2D-003",
    },
    "tilemap.tile_unknown": {
        code: "tilemap.tile_unknown",
        message: "A tile id falls in no tile-set's [firstTileId, firstTileId + tileCount) global range (tile 0 = empty is always valid \u2014 an erase is a paint with 0).",
        retriable: false,
        exitCode: 5,
        origin: "R-2D-003",
    },
    "attach.denied": {
        code: "attach.denied",
        message: "The attach token is missing or does not match the daemon's instance token; the attach is refused (attach-token enforcement, D20 / R-SEC-002).",
        retriable: false,
        exitCode: 6,
        origin: "R-SEC-002",
    },
    "daemon.busy": {
        code: "daemon.busy",
        message: "The daemon is already serving its maximum number of concurrent clients; the attach is refused. Transient \u2014 a slot frees when a client detaches, so a later retry can succeed.",
        retriable: true,
        exitCode: 2,
        origin: "R-BRIDGE-001",
    },
    "editor.session_state_invalid": {
        code: "editor.session_state_invalid",
        message: "The daemon's editor session file (.editor/session.json) is malformed or an unsupported version; it was renamed aside and the session state was reset to defaults.",
        retriable: false,
        exitCode: 5,
        origin: "R-BRIDGE-008",
    },
};

/** Every error code, in catalog order. */
export const ERROR_CODES: readonly ErrorCode[] = ["usage.invalid", "usage.unknown_verb", "usage.unknown_flag", "usage.missing_argument", "namespace.collision", "file.not_found", "file.parse_error", "file.validation_failed", "cas.mismatch", "path.jail_violation", "handshake.incompatible_protocol", "version.mismatch", "package.engine_incompatible", "schema.newer_than_engine", "schema.newer_than_package", "contract.unimplemented", "internal.error", "scope.denied", "merge.conflict", "merge.id_conflict", "merge.binary_sidecar", "merge.meta_guid", "merge.newer_stamped", "merge.duplicate_id", "merge.no_conflict_at_path", "merge.rekey_target_invalid", "scope.insufficient", "contract.operational_only", "resource.unknown_handle", "migration.step_missing", "migration.step_failed", "migration.budget_exceeded", "migration.id_mutated", "migration.runner_unavailable", "migration.orphan_override", "sidecar.bad_magic", "sidecar.truncated", "sidecar.unsupported_version", "sidecar.ref_malformed", "sidecar.dangling_ref", "sidecar.orphaned", "sidecar.hash_mismatch", "asset.guid_duplicate", "asset.meta_orphaned", "asset.meta_invalid", "asset.heal_ambiguous", "asset.ref_dangling", "asset.ref_path_only", "asset.ref_hint_stale", "asset.move_source_missing", "asset.move_destination_exists", "asset.move_invalid", "import.source_malformed", "import.decode_failed", "import.unsupported_format", "import.jail_escape", "import.subprocess_failed", "import.non_deterministic", "import.cache_corrupt", "compose.write_target_not_found", "compose.immutable_pointer", "tilemap.chunk_oversize", "tilemap.region_invalid", "tilemap.id_duplicate", "stringtable.locale_duplicate", "stringtable.key_duplicate", "stringtable.fallback_unknown", "stringtable.fallback_cycle", "stringtable.value_invalid", "stringtable.plural_incomplete", "stringtable.value_locale_duplicate", "save.malformed", "save.unknown_component", "save.back_compat_exceeded", "save.format_unsupported", "session.state_invalid", "session.state_not_found", "session.input_invalid", "replay.artifact_invalid", "replay.manifest_drift", "replay.divergence", "query.syntax_error", "query.unknown_operator", "query.invalid_cursor", "query.unsupported_surface", "ts.transpile_failed", "ts.bundle_failed", "ts.type_error", "ts.runtime_error", "subscription.unknown_sub", "install.version_unpinned", "install.integrity_mismatch", "install.lockfile_incomplete", "install.scripts_required", "install.fetch_failed", "consent_required", "debug.attach_failed", "debug.unsupported", "merge.invalid_stable_id", "viewport.adapter_absent", "viewport.surface_unavailable", "viewport.render_failed", "play.not_running", "play.session_failed", "play.step_failed", "play.hot_reload_failed", "determinism.attestation_fastmath_forbidden", "determinism.attestation_strict_fp_missing", "determinism.attestation_flags_unverified", "physics3d.invalid_entity", "physics3d.missing_component", "physics3d.invalid_shape", "physics3d.invalid_mass", "physics3d.invalid_step", "physics2d.invalid_entity", "physics2d.missing_component", "physics2d.invalid_shape", "physics2d.invalid_mass", "physics2d.invalid_step", "anim.invalid_entity", "anim.missing_component", "anim.invalid_rig", "anim.duplicate_component", "anim.invalid_step", "particle.invalid_entity", "particle.missing_component", "particle.invalid_config", "particle.invalid_step", "spline.invalid_entity", "spline.missing_component", "spline.invalid_path", "spline.duplicate_component", "spline.invalid_step", "audio.invalid_entity", "audio.invalid_bus", "audio.invalid_event", "audio.device_unavailable", "input.invalid_context", "input.duplicate_context", "input.unknown_context", "input.unknown_action", "sim.gc.unavailable", "sim.gc.invalid_budget", "sim.gc.window_failed", "net.invalid_net_id", "net.duplicate_net_id", "net.snapshot_component_mismatch", "net.authority_conflict", "ui.scene_not_found", "ui.scene_invalid", "ui.node_not_found", "ui.invalid_event", "ui.assertion_failed", "transcode.no_target", "transcode.unsupported_format", "transcode.bad_descriptor", "build.template_unverified", "build.toolchain_fetch_failed", "build.aot_failed", "build.transcode_failed", "build.link_failed", "doctor.environment_incomplete", "doctor.toolchain_missing", "doctor.toolchain_version_mismatch", "doctor.filesync_budget_low", "doctor.signing_prereq_absent", "doctor.unknown_target", "build.artifact_unsigned", "tilemap.layer_not_found", "tilemap.cell_out_of_bounds", "tilemap.tile_unknown", "attach.denied", "daemon.busy", "editor.session_state_invalid"];
