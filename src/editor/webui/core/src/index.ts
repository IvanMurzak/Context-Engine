// @context-engine/editor-core — the editor's web app entry point (design 04 section 1).
//
// e05a scope is the TOOLCHAIN SUBSTRATE only: this module exists so the workspace has a real,
// type-checked, bundleable entry that proves the whole chain end to end (pinned esbuild bundle +
// pinned tsgo typecheck + registry-generated client typings). There is deliberately NO app
// behaviour yet — the app scheme and IPC bridge land in e05c, and PanelHost/dockview wiring plus
// the hydration runtime land in e05c/e05d. Do not grow this file into the app; grow it into those.

// Imported for use in THIS module's own helpers below; the whole generated surface is re-exported
// wholesale further down, so this list is deliberately only what the function bodies need.
import {
    CLIENT_SCHEMA_VERSION,
    ERROR_CATALOG,
    ERROR_CODES,
    EVENT_TOPICS,
    EVENT_TOPIC_NAMES,
    MIN_CLIENT_PROTOCOL,
    PROTOCOL_MAJOR,
    RPC_METHODS,
    RPC_METHOD_NAMES,
} from "./generated/client-schema.js";
import type {
    ErrorCode,
    EventTopic,
    EventTopicDescriptor,
    RpcMethod,
    RpcMethodDescriptor,
} from "./generated/client-schema.js";

// Re-export the WHOLE generated surface so consumers (e05c's IPC bridge, e05d's panels) import the
// contract from ONE place and can never hand-roll a divergent copy. `export *` makes that
// exhaustiveness STRUCTURAL rather than a promise: a hand-listed re-export block is a mirror of a
// DO-NOT-EDIT-BY-HAND generated file, so it falls behind the moment the generator emits a new
// symbol, and the failure is SILENT — a consumer simply reaches past this barrel into
// ./generated/client-schema.js, which is the divergent second import path the barrel exists to deny.
export * from "./generated/client-schema.js";

/** Build-time facts about this editor-core bundle and the contract surface it was generated against. */
export interface EditorCoreInfo {
    readonly protocolMajor: typeof PROTOCOL_MAJOR;
    readonly minClientProtocol: typeof MIN_CLIENT_PROTOCOL;
    readonly clientSchemaVersion: typeof CLIENT_SCHEMA_VERSION;
    readonly rpcMethodCount: number;
    readonly eventTopicCount: number;
    readonly errorCodeCount: number;
}

/** Report the contract surface this bundle was built against (the bundle's self-description). */
export function editorCoreInfo(): EditorCoreInfo {
    return {
        protocolMajor: PROTOCOL_MAJOR,
        minClientProtocol: MIN_CLIENT_PROTOCOL,
        clientSchemaVersion: CLIENT_SCHEMA_VERSION,
        rpcMethodCount: RPC_METHOD_NAMES.length,
        eventTopicCount: EVENT_TOPIC_NAMES.length,
        errorCodeCount: ERROR_CODES.length,
    };
}

/**
 * Narrow an arbitrary string to a known RPC method.
 *
 * The daemon is the authority on what exists; this is the client-side guard that keeps an unknown
 * verb from being sent as if it were real. Generated-union-backed, so a registry change
 * automatically changes what this accepts.
 */
export function isRpcMethod(value: string): value is RpcMethod {
    return Object.prototype.hasOwnProperty.call(RPC_METHODS, value);
}

/** Narrow an arbitrary string to a known event topic (same rationale as `isRpcMethod`). */
export function isEventTopic(value: string): value is EventTopic {
    return Object.prototype.hasOwnProperty.call(EVENT_TOPICS, value);
}

/** Narrow an arbitrary string to a known catalog error code. */
export function isErrorCode(value: string): value is ErrorCode {
    return Object.prototype.hasOwnProperty.call(ERROR_CATALOG, value);
}

/**
 * Whether a failed request carrying `code` may be retried unchanged.
 *
 * Reads the registry's own `retriable` fact rather than a client-side guess — an unknown code is
 * conservatively NOT retriable (a client must never invent retry semantics for a surface it does
 * not understand).
 */
export function isRetriable(code: string): boolean {
    return isErrorCode(code) ? ERROR_CATALOG[code].retriable : false;
}

/** Look up a method's registry description, or `undefined` when the method is not published. */
export function describeRpcMethod(method: string): RpcMethodDescriptor | undefined {
    return isRpcMethod(method) ? RPC_METHODS[method] : undefined;
}

/** Look up a topic's registry description, or `undefined` when the topic is not published. */
export function describeEventTopic(topic: string): EventTopicDescriptor | undefined {
    return isEventTopic(topic) ? EVENT_TOPICS[topic] : undefined;
}
