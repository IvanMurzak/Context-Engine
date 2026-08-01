// THE CAPABILITY GRANT SOURCE, editor-core's half (M9 e13c-4, design 08 §2-§3 / 04 §3/§5).
//
// WHAT THIS MODULE IS. `panelverbs.ts` § the capability gate declares a one-method interface —
// `PanelCapabilityGrants` — and ships exactly one implementation of it, `DENY_ALL_CAPABILITY_GRANTS`,
// with the standing note that "e13c replaces THAT ONE OBJECT with install-consent grants". This is
// that replacement, and it is deliberately the whole of the editor-core-side change: the ENFORCEMENT
// point (`requireCapability`, one call site) is untouched, the refusal shape is untouched, and this
// module contains no gate of its own.
//
// ⚠ THE GRANT IS THE SHELL'S ANSWER, NEVER THE MANIFEST'S. `PanelManifest.capabilities` is what a
// package ASKED for — a file the package itself authors — so reading it as the grant would let any
// package grant itself `ui_events` by editing its own bundle. The values here come from
// `package.grants.list`, whose C++ half (`package_grants.h`) reads a document under `~/.context/` that
// no package can write and clamps every answer to what that package's manifest declared. Editor-core
// therefore never decides a grant; it only carries one, and a bug in this file can only DENY.
//
// ⚠ FAIL-CLOSED IN EVERY DIRECTION, and that is the load-bearing property. A Shell that does not serve
// the route (an older build, a secondary window whose router lacks the surface — the same in-tree gap
// `packageevents.ts` records), a reply that is not the shape we expect, a member of the wrong type: all
// of them yield NO grants, which is byte-for-byte the `DENY_ALL_CAPABILITY_GRANTS` behaviour this
// module replaces. The direction matters — the opposite default would hand third-party code scopes
// nobody consented to on exactly the paths nobody tests.
//
// ⚠ A SNAPSHOT, NOT A LIVE VIEW, and the choice is deliberate. The grants are read ONCE at boot and
// held for the life of the window. A consent answer given mid-session therefore takes effect on the
// next boot rather than under a mounted panel — the honest shape, because a grant that widened live
// would have to reach a panel that has already been told what it may do, and a grant that NARROWED
// live would leave a panel holding a port it was granted under the old answer. The C++ side applies a
// decision to the running Shell immediately (that is where the daemon session lives); this side does
// not pretend to.

import { BridgeError, isRecord } from "./bridge.js";
import type { ShellBridge } from "./bridge.js";
import type { PanelCapabilityGrants } from "./panelverbs.js";

/**
 * The Shell method this reads (M9 e13c-4).
 *
 * MIRRORS C++ `kPackageGrantsListMethod`
 * (`src/editor/shell/include/context/editor/shell/package_grants.h`), exactly as
 * `PANEL_EVENTS_POLL_METHOD` mirrors `kPanelEventsPollMethod`. A rename on either side leaves
 * editor-core asking for a method the Shell no longer routes — and here the consequence is that every
 * package is DENIED, which is the direction to fail in but is still a silent one, so the mirror is
 * stated on both sides rather than assumed.
 */
export const PACKAGE_GRANTS_LIST_METHOD = "package.grants.list";

/**
 * The Shell method a consent surface answers through (M9 e13c-4). MIRRORS C++
 * `kPackageGrantsDecideMethod`.
 *
 * Named here rather than only C++-side because the consent UI is editor-core's to build and this is
 * the one route it may use: recording a decision is a WRITE to a security document, so it goes through
 * the Shell — which owns the file, clamps the answer to the manifest, and persists it atomically —
 * never through anything on this side.
 */
export const PACKAGE_GRANTS_DECIDE_METHOD = "package.grants.decide";

/** One package's consent state, as the Shell reports it. */
export interface PackageGrantRecord {
    readonly id: string;
    readonly version: string;
    /** What the package's MANIFEST asked for — a DECLARATION. Diagnostics + the consent listing. */
    readonly requested: readonly string[];
    /** What the operator actually consented to, already clamped to `requested` by the Shell. */
    readonly granted: readonly string[];
    /** Has anyone answered at all? FALSE ⇒ a consent prompt is still owed for this package. */
    readonly decided: boolean;
}

/** Read one entry of a `package.grants.list` reply. `null` for anything that is not the shape. */
function parseRecord(value: unknown): PackageGrantRecord | null {
    if (!isRecord(value) || typeof value["id"] !== "string" || value["id"] === "") {
        return null;
    }
    // FAIL-CLOSED PER MEMBER. `granted` is the only member that can WIDEN anything, so a `granted`
    // that is not an array of strings makes the whole record unusable rather than being defaulted to
    // an empty list — the two outcomes are the same today, and they would stop being the same the
    // moment someone "helpfully" defaulted a missing member the other way.
    const granted = value["granted"];
    const requested = value["requested"];
    if (!isStringArray(granted) || !isStringArray(requested)) {
        return null;
    }
    return {
        id: value["id"],
        version: typeof value["version"] === "string" ? value["version"] : "",
        requested,
        granted,
        decided: value["decided"] === true,
    };
}

function isStringArray(value: unknown): value is readonly string[] {
    return Array.isArray(value) && value.every((entry) => typeof entry === "string");
}

/**
 * Read a whole `package.grants.list` reply. `null` when it is not the shape at all.
 *
 * ⚠ ONE MALFORMED ENTRY DISCARDS THE WHOLE REPLY, rather than being skipped. Skipping would produce a
 * PARTIAL grant table that looks healthy, and the failure it hides is a version skew between the Shell
 * and this bundle — exactly the state in which "some packages are mysteriously denied" is the report a
 * human would have to debug. All-or-nothing makes the skew total and therefore visible.
 */
export function parsePackageGrants(value: unknown): PackageGrantRecord[] | null {
    if (!isRecord(value) || !Array.isArray(value["packages"])) {
        return null;
    }
    const records: PackageGrantRecord[] = [];
    for (const entry of value["packages"]) {
        const record = parseRecord(entry);
        if (record === null) {
            return null;
        }
        records.push(record);
    }
    return records;
}

/**
 * The grant source `makePanelBridgeVerbs` consults — the object that replaces
 * `DENY_ALL_CAPABILITY_GRANTS` in the shipping editor.
 *
 * IMMUTABLE ONCE BUILT. There is no `grant()` on this class and no setter: the only way to obtain one
 * that answers TRUE for anything is to have read it out of the Shell's own reply, so nothing in
 * editor-core can widen a package's authority by holding a reference to this object (the property
 * `DENY_ALL_CAPABILITY_GRANTS` gets from `Object.freeze`, kept).
 */
export class ShellPackageGrants implements PanelCapabilityGrants {
    readonly #byPackage: ReadonlyMap<string, ReadonlySet<string>>;
    readonly #records: readonly PackageGrantRecord[];

    private constructor(records: readonly PackageGrantRecord[]) {
        const table = new Map<string, ReadonlySet<string>>();
        for (const record of records) {
            table.set(record.id, new Set(record.granted));
        }
        this.#byPackage = table;
        this.#records = records;
    }

    /** A source that grants NOTHING — the deny-all state every failure path lands on. */
    static empty(): ShellPackageGrants {
        return new ShellPackageGrants([]);
    }

    /** Build from already-parsed records (the T1 tier's entry point, and `load`'s tail). */
    static fromRecords(records: readonly PackageGrantRecord[]): ShellPackageGrants {
        return new ShellPackageGrants(records);
    }

    /**
     * Read the grants from the Shell. NEVER REJECTS — every failure resolves to `empty()`.
     *
     * It runs on the boot path, whose only diagnostic channel is a DOM attribute, so an escaping
     * rejection is an unhandled error nobody sees AND a boot that stops before the panels mount. A
     * `BridgeError` (no route, a refused call) and a transport fault are the same answer here — "we
     * could not learn of any grant" — because there is no third behaviour that is safe.
     */
    static async load(bridge: ShellBridge): Promise<ShellPackageGrants> {
        let reply: unknown;
        try {
            reply = await bridge.call(PACKAGE_GRANTS_LIST_METHOD);
        } catch (error) {
            // Named rather than collapsed so a reader knows both arms were considered; both deny.
            void (error instanceof BridgeError);
            return ShellPackageGrants.empty();
        }
        const records = parsePackageGrants(reply);
        return records === null ? ShellPackageGrants.empty() : new ShellPackageGrants(records);
    }

    /** Everything the Shell reported — what a consent surface renders. */
    get records(): readonly PackageGrantRecord[] {
        return this.#records;
    }

    /** THE `PanelCapabilityGrants` ANSWER. False for every unknown package and unknown capability. */
    granted(packageId: string, capability: string): boolean {
        return this.#byPackage.get(packageId)?.has(capability) === true;
    }
}
