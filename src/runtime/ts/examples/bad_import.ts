// Deliberately unresolved import — a BUNDLE-error fixture. Under esbuild `--bundle` the missing
// module makes esbuild exit non-zero, which the toolchain surfaces as a `ts.bundle_failed`
// diagnostic (R-CLI-008), distinct from a plain transpile syntax error. NOT a build input.
import { missing } from "./does_not_exist";

export const value: number = missing;
