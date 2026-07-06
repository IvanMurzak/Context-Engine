// Rust-side stub definitions for the rusty_v8 prebuilt archive (issue #76 / task 2a).
//
// AUTHORITATIVE, DETERMINISTIC, BOUNDED set - do NOT hand-edit. It is the exact set of symbols
// the pinned librusty_v8.a / rusty_v8.lib (tools/v8-prebuilt.json: rusty_v8 v149.4.0 / V8
// 14.9.207.2) references but does NOT define, because rusty_v8's prebuilt is built to be
// consumed FROM RUST: its C++ object files call extern-"C" symbols that the Rust `v8` crate
// (and its temporal_rs / cppgc dependencies) provide. A pure-C++ / hybrid embedder that links
// ONLY the archive must satisfy them itself. Enumerated by `nm` over the SHA-pinned archive
// (union verified identical across the Linux-x64 / macOS-ARM64 / Win-x64 CI build legs - see
// PR #82), so the set is closed and reproducible from the pin: re-derive after any bump of
// tools/v8-prebuilt.json (a new rusty_v8 version can shift the set).
//
// WHY STUBS ARE SAFE HERE (the minimal host never reaches them at runtime):
//   * v8__Platform__CustomPlatform__BASE__*  - the vtable thunks a *Rust-side* custom platform
//     forwards to. This host boots the real C++ default platform via
//     v8__Platform__NewDefaultPlatform (v8_shims.h); it never constructs a rusty_v8 custom
//     platform, so these are unreachable.
//   * v8_inspector__V8Inspector{,Client}__BASE__*  - the rusty_v8 Rust-side inspector trait
//     thunks. The R-OBS-005 seam (inspector_seam.h) derives from the REAL C++
//     v8_inspector::V8InspectorClient, NOT rusty_v8's BASE wrapper, and no channel/debugger is
//     built in task 2a - unreachable.
//   * v8__Value{,De}Serializer__Delegate__*  - structured-clone serializer delegate thunks;
//     this host never serializes - unreachable.
//   * rusty_v8_RustObj_{drop,trace,get_name}  - the cppgc lifecycle callbacks for Rust-managed
//     GC objects; this host creates none - unreachable.
//   * temporal_rs_*  - the ICU4X-backed Temporal / Intl.DateTimeFormat C-ABI implementations,
//     force-included via V8's builtins table (builtins-temporal.o / js-temporal-*.o /
//     js-date-time-format.o are pulled in by Isolate/snapshot setup) but only CALLED when JS
//     executes Temporal.* (or Intl calendar/timezone ops). Task 2a's eval surface never touches
//     Temporal/Intl, so they are linked-but-unreached.
//
// Each stub is a fail-closed TRAP (not a silent no-op): if the never-reached assumption is ever
// violated, the process aborts LOUDLY with the exact symbol name, turning a latent corruption
// into an unambiguous CI signal (and clean escalation evidence). Real implementations belong to
// the from-source V8 migration seam (README.md, "From-source migration seam"), which links the
// Rust `v8` crate + temporal_rs proper - explicitly out of task-2a scope.
//
// Compiled ONLY into the V8 backend (CONTEXT_JS_HAS_V8 - the 3-OS CI build legs); the local
// Strawberry-GCC stub backend does not link the archive and does not need these.

#include <cstdio>
#include <cstdlib>

extern "C"
{

static void ctx_v8_rust_stub_trap(const char* symbol)
{
    std::fprintf(stderr,
                 "FATAL: rusty_v8 Rust-side stub '%s' was invoked. The minimal in-process V8 "
                 "host (issue #76 / task 2a) must never reach it (no custom platform, no "
                 "inspector channel, no serializer, no Temporal/Intl execution). See "
                 "src/runtime/js/src/v8_rust_stubs.cpp.\n",
                 symbol);
    std::fflush(stderr);
    std::abort();
}


    // --- v8::Platform::CustomPlatform BASE thunks (Rust-side custom platform vtable) - unreached (6) ---
    void v8__Platform__CustomPlatform__BASE__DROP(void) { ctx_v8_rust_stub_trap("v8__Platform__CustomPlatform__BASE__DROP"); }
    void v8__Platform__CustomPlatform__BASE__PostDelayedTask(void) { ctx_v8_rust_stub_trap("v8__Platform__CustomPlatform__BASE__PostDelayedTask"); }
    void v8__Platform__CustomPlatform__BASE__PostIdleTask(void) { ctx_v8_rust_stub_trap("v8__Platform__CustomPlatform__BASE__PostIdleTask"); }
    void v8__Platform__CustomPlatform__BASE__PostNonNestableDelayedTask(void) { ctx_v8_rust_stub_trap("v8__Platform__CustomPlatform__BASE__PostNonNestableDelayedTask"); }
    void v8__Platform__CustomPlatform__BASE__PostNonNestableTask(void) { ctx_v8_rust_stub_trap("v8__Platform__CustomPlatform__BASE__PostNonNestableTask"); }
    void v8__Platform__CustomPlatform__BASE__PostTask(void) { ctx_v8_rust_stub_trap("v8__Platform__CustomPlatform__BASE__PostTask"); }

    // --- v8_inspector V8Inspector/V8InspectorClient BASE thunks (Rust-side inspector) - unreached (10) ---
    void v8_inspector__V8InspectorClient__BASE__consoleAPIMessage(void) { ctx_v8_rust_stub_trap("v8_inspector__V8InspectorClient__BASE__consoleAPIMessage"); }
    void v8_inspector__V8InspectorClient__BASE__ensureDefaultContextInGroup(void) { ctx_v8_rust_stub_trap("v8_inspector__V8InspectorClient__BASE__ensureDefaultContextInGroup"); }
    void v8_inspector__V8InspectorClient__BASE__generateUniqueId(void) { ctx_v8_rust_stub_trap("v8_inspector__V8InspectorClient__BASE__generateUniqueId"); }
    void v8_inspector__V8InspectorClient__BASE__quitMessageLoopOnPause(void) { ctx_v8_rust_stub_trap("v8_inspector__V8InspectorClient__BASE__quitMessageLoopOnPause"); }
    void v8_inspector__V8InspectorClient__BASE__resourceNameToUrl(void) { ctx_v8_rust_stub_trap("v8_inspector__V8InspectorClient__BASE__resourceNameToUrl"); }
    void v8_inspector__V8InspectorClient__BASE__runIfWaitingForDebugger(void) { ctx_v8_rust_stub_trap("v8_inspector__V8InspectorClient__BASE__runIfWaitingForDebugger"); }
    void v8_inspector__V8InspectorClient__BASE__runMessageLoopOnPause(void) { ctx_v8_rust_stub_trap("v8_inspector__V8InspectorClient__BASE__runMessageLoopOnPause"); }
    void v8_inspector__V8Inspector__Channel__BASE__flushProtocolNotifications(void) { ctx_v8_rust_stub_trap("v8_inspector__V8Inspector__Channel__BASE__flushProtocolNotifications"); }
    void v8_inspector__V8Inspector__Channel__BASE__sendNotification(void) { ctx_v8_rust_stub_trap("v8_inspector__V8Inspector__Channel__BASE__sendNotification"); }
    void v8_inspector__V8Inspector__Channel__BASE__sendResponse(void) { ctx_v8_rust_stub_trap("v8_inspector__V8Inspector__Channel__BASE__sendResponse"); }

    // --- v8::Value{,De}Serializer::Delegate thunks (structured clone) - unreached (11) ---
    void v8__ValueDeserializer__Delegate__GetSharedArrayBufferFromId(void) { ctx_v8_rust_stub_trap("v8__ValueDeserializer__Delegate__GetSharedArrayBufferFromId"); }
    void v8__ValueDeserializer__Delegate__GetWasmModuleFromId(void) { ctx_v8_rust_stub_trap("v8__ValueDeserializer__Delegate__GetWasmModuleFromId"); }
    void v8__ValueDeserializer__Delegate__ReadHostObject(void) { ctx_v8_rust_stub_trap("v8__ValueDeserializer__Delegate__ReadHostObject"); }
    void v8__ValueSerializer__Delegate__FreeBufferMemory(void) { ctx_v8_rust_stub_trap("v8__ValueSerializer__Delegate__FreeBufferMemory"); }
    void v8__ValueSerializer__Delegate__GetSharedArrayBufferId(void) { ctx_v8_rust_stub_trap("v8__ValueSerializer__Delegate__GetSharedArrayBufferId"); }
    void v8__ValueSerializer__Delegate__GetWasmModuleTransferId(void) { ctx_v8_rust_stub_trap("v8__ValueSerializer__Delegate__GetWasmModuleTransferId"); }
    void v8__ValueSerializer__Delegate__HasCustomHostObject(void) { ctx_v8_rust_stub_trap("v8__ValueSerializer__Delegate__HasCustomHostObject"); }
    void v8__ValueSerializer__Delegate__IsHostObject(void) { ctx_v8_rust_stub_trap("v8__ValueSerializer__Delegate__IsHostObject"); }
    void v8__ValueSerializer__Delegate__ReallocateBufferMemory(void) { ctx_v8_rust_stub_trap("v8__ValueSerializer__Delegate__ReallocateBufferMemory"); }
    void v8__ValueSerializer__Delegate__ThrowDataCloneError(void) { ctx_v8_rust_stub_trap("v8__ValueSerializer__Delegate__ThrowDataCloneError"); }
    void v8__ValueSerializer__Delegate__WriteHostObject(void) { ctx_v8_rust_stub_trap("v8__ValueSerializer__Delegate__WriteHostObject"); }

    // --- cppgc RustObj lifecycle callbacks - unreached (3) ---
    void rusty_v8_RustObj_drop(void) { ctx_v8_rust_stub_trap("rusty_v8_RustObj_drop"); }
    void rusty_v8_RustObj_get_name(void) { ctx_v8_rust_stub_trap("rusty_v8_RustObj_get_name"); }
    void rusty_v8_RustObj_trace(void) { ctx_v8_rust_stub_trap("rusty_v8_RustObj_trace"); }

    // --- temporal_rs_* ICU4X Temporal/Intl C-ABI (force-included via builtins table) - unreached (258) ---
    void temporal_rs_AnyCalendarKind_get_for_str(void) { ctx_v8_rust_stub_trap("temporal_rs_AnyCalendarKind_get_for_str"); }
    void temporal_rs_AnyCalendarKind_parse_temporal_calendar_string(void) { ctx_v8_rust_stub_trap("temporal_rs_AnyCalendarKind_parse_temporal_calendar_string"); }
    void temporal_rs_Calendar_identifier(void) { ctx_v8_rust_stub_trap("temporal_rs_Calendar_identifier"); }
    void temporal_rs_Calendar_kind(void) { ctx_v8_rust_stub_trap("temporal_rs_Calendar_kind"); }
    void temporal_rs_Duration_abs(void) { ctx_v8_rust_stub_trap("temporal_rs_Duration_abs"); }
    void temporal_rs_Duration_add(void) { ctx_v8_rust_stub_trap("temporal_rs_Duration_add"); }
    void temporal_rs_Duration_clone(void) { ctx_v8_rust_stub_trap("temporal_rs_Duration_clone"); }
    void temporal_rs_Duration_compare_with_provider(void) { ctx_v8_rust_stub_trap("temporal_rs_Duration_compare_with_provider"); }
    void temporal_rs_Duration_create(void) { ctx_v8_rust_stub_trap("temporal_rs_Duration_create"); }
    void temporal_rs_Duration_days(void) { ctx_v8_rust_stub_trap("temporal_rs_Duration_days"); }
    void temporal_rs_Duration_destroy(void) { ctx_v8_rust_stub_trap("temporal_rs_Duration_destroy"); }
    void temporal_rs_Duration_from_partial_duration(void) { ctx_v8_rust_stub_trap("temporal_rs_Duration_from_partial_duration"); }
    void temporal_rs_Duration_from_utf16(void) { ctx_v8_rust_stub_trap("temporal_rs_Duration_from_utf16"); }
    void temporal_rs_Duration_from_utf8(void) { ctx_v8_rust_stub_trap("temporal_rs_Duration_from_utf8"); }
    void temporal_rs_Duration_hours(void) { ctx_v8_rust_stub_trap("temporal_rs_Duration_hours"); }
    void temporal_rs_Duration_microseconds(void) { ctx_v8_rust_stub_trap("temporal_rs_Duration_microseconds"); }
    void temporal_rs_Duration_milliseconds(void) { ctx_v8_rust_stub_trap("temporal_rs_Duration_milliseconds"); }
    void temporal_rs_Duration_minutes(void) { ctx_v8_rust_stub_trap("temporal_rs_Duration_minutes"); }
    void temporal_rs_Duration_months(void) { ctx_v8_rust_stub_trap("temporal_rs_Duration_months"); }
    void temporal_rs_Duration_nanoseconds(void) { ctx_v8_rust_stub_trap("temporal_rs_Duration_nanoseconds"); }
    void temporal_rs_Duration_negated(void) { ctx_v8_rust_stub_trap("temporal_rs_Duration_negated"); }
    void temporal_rs_Duration_round_with_provider(void) { ctx_v8_rust_stub_trap("temporal_rs_Duration_round_with_provider"); }
    void temporal_rs_Duration_seconds(void) { ctx_v8_rust_stub_trap("temporal_rs_Duration_seconds"); }
    void temporal_rs_Duration_sign(void) { ctx_v8_rust_stub_trap("temporal_rs_Duration_sign"); }
    void temporal_rs_Duration_subtract(void) { ctx_v8_rust_stub_trap("temporal_rs_Duration_subtract"); }
    void temporal_rs_Duration_to_string(void) { ctx_v8_rust_stub_trap("temporal_rs_Duration_to_string"); }
    void temporal_rs_Duration_total_with_provider(void) { ctx_v8_rust_stub_trap("temporal_rs_Duration_total_with_provider"); }
    void temporal_rs_Duration_weeks(void) { ctx_v8_rust_stub_trap("temporal_rs_Duration_weeks"); }
    void temporal_rs_Duration_years(void) { ctx_v8_rust_stub_trap("temporal_rs_Duration_years"); }
    void temporal_rs_I128Nanoseconds_is_valid(void) { ctx_v8_rust_stub_trap("temporal_rs_I128Nanoseconds_is_valid"); }
    void temporal_rs_Instant_add(void) { ctx_v8_rust_stub_trap("temporal_rs_Instant_add"); }
    void temporal_rs_Instant_clone(void) { ctx_v8_rust_stub_trap("temporal_rs_Instant_clone"); }
    void temporal_rs_Instant_compare(void) { ctx_v8_rust_stub_trap("temporal_rs_Instant_compare"); }
    void temporal_rs_Instant_destroy(void) { ctx_v8_rust_stub_trap("temporal_rs_Instant_destroy"); }
    void temporal_rs_Instant_epoch_milliseconds(void) { ctx_v8_rust_stub_trap("temporal_rs_Instant_epoch_milliseconds"); }
    void temporal_rs_Instant_epoch_nanoseconds(void) { ctx_v8_rust_stub_trap("temporal_rs_Instant_epoch_nanoseconds"); }
    void temporal_rs_Instant_from_epoch_milliseconds(void) { ctx_v8_rust_stub_trap("temporal_rs_Instant_from_epoch_milliseconds"); }
    void temporal_rs_Instant_from_utf16(void) { ctx_v8_rust_stub_trap("temporal_rs_Instant_from_utf16"); }
    void temporal_rs_Instant_from_utf8(void) { ctx_v8_rust_stub_trap("temporal_rs_Instant_from_utf8"); }
    void temporal_rs_Instant_round(void) { ctx_v8_rust_stub_trap("temporal_rs_Instant_round"); }
    void temporal_rs_Instant_since(void) { ctx_v8_rust_stub_trap("temporal_rs_Instant_since"); }
    void temporal_rs_Instant_subtract(void) { ctx_v8_rust_stub_trap("temporal_rs_Instant_subtract"); }
    void temporal_rs_Instant_to_ixdtf_string_with_provider(void) { ctx_v8_rust_stub_trap("temporal_rs_Instant_to_ixdtf_string_with_provider"); }
    void temporal_rs_Instant_to_zoned_date_time_iso_with_provider(void) { ctx_v8_rust_stub_trap("temporal_rs_Instant_to_zoned_date_time_iso_with_provider"); }
    void temporal_rs_Instant_try_new(void) { ctx_v8_rust_stub_trap("temporal_rs_Instant_try_new"); }
    void temporal_rs_Instant_until(void) { ctx_v8_rust_stub_trap("temporal_rs_Instant_until"); }
    void temporal_rs_OwnedRelativeTo_from_utf16_with_provider(void) { ctx_v8_rust_stub_trap("temporal_rs_OwnedRelativeTo_from_utf16_with_provider"); }
    void temporal_rs_OwnedRelativeTo_from_utf8_with_provider(void) { ctx_v8_rust_stub_trap("temporal_rs_OwnedRelativeTo_from_utf8_with_provider"); }
    void temporal_rs_ParsedDateTime_destroy(void) { ctx_v8_rust_stub_trap("temporal_rs_ParsedDateTime_destroy"); }
    void temporal_rs_ParsedDateTime_from_utf16(void) { ctx_v8_rust_stub_trap("temporal_rs_ParsedDateTime_from_utf16"); }
    void temporal_rs_ParsedDateTime_from_utf8(void) { ctx_v8_rust_stub_trap("temporal_rs_ParsedDateTime_from_utf8"); }
    void temporal_rs_ParsedDate_destroy(void) { ctx_v8_rust_stub_trap("temporal_rs_ParsedDate_destroy"); }
    void temporal_rs_ParsedDate_from_utf16(void) { ctx_v8_rust_stub_trap("temporal_rs_ParsedDate_from_utf16"); }
    void temporal_rs_ParsedDate_from_utf8(void) { ctx_v8_rust_stub_trap("temporal_rs_ParsedDate_from_utf8"); }
    void temporal_rs_ParsedDate_month_day_from_utf16(void) { ctx_v8_rust_stub_trap("temporal_rs_ParsedDate_month_day_from_utf16"); }
    void temporal_rs_ParsedDate_month_day_from_utf8(void) { ctx_v8_rust_stub_trap("temporal_rs_ParsedDate_month_day_from_utf8"); }
    void temporal_rs_ParsedDate_year_month_from_utf16(void) { ctx_v8_rust_stub_trap("temporal_rs_ParsedDate_year_month_from_utf16"); }
    void temporal_rs_ParsedDate_year_month_from_utf8(void) { ctx_v8_rust_stub_trap("temporal_rs_ParsedDate_year_month_from_utf8"); }
    void temporal_rs_ParsedZonedDateTime_destroy(void) { ctx_v8_rust_stub_trap("temporal_rs_ParsedZonedDateTime_destroy"); }
    void temporal_rs_ParsedZonedDateTime_from_utf16_with_provider(void) { ctx_v8_rust_stub_trap("temporal_rs_ParsedZonedDateTime_from_utf16_with_provider"); }
    void temporal_rs_ParsedZonedDateTime_from_utf8_with_provider(void) { ctx_v8_rust_stub_trap("temporal_rs_ParsedZonedDateTime_from_utf8_with_provider"); }
    void temporal_rs_PlainDateTime_add(void) { ctx_v8_rust_stub_trap("temporal_rs_PlainDateTime_add"); }
    void temporal_rs_PlainDateTime_calendar(void) { ctx_v8_rust_stub_trap("temporal_rs_PlainDateTime_calendar"); }
    void temporal_rs_PlainDateTime_clone(void) { ctx_v8_rust_stub_trap("temporal_rs_PlainDateTime_clone"); }
    void temporal_rs_PlainDateTime_compare(void) { ctx_v8_rust_stub_trap("temporal_rs_PlainDateTime_compare"); }
    void temporal_rs_PlainDateTime_day(void) { ctx_v8_rust_stub_trap("temporal_rs_PlainDateTime_day"); }
    void temporal_rs_PlainDateTime_day_of_week(void) { ctx_v8_rust_stub_trap("temporal_rs_PlainDateTime_day_of_week"); }
    void temporal_rs_PlainDateTime_day_of_year(void) { ctx_v8_rust_stub_trap("temporal_rs_PlainDateTime_day_of_year"); }
    void temporal_rs_PlainDateTime_days_in_month(void) { ctx_v8_rust_stub_trap("temporal_rs_PlainDateTime_days_in_month"); }
    void temporal_rs_PlainDateTime_days_in_week(void) { ctx_v8_rust_stub_trap("temporal_rs_PlainDateTime_days_in_week"); }
    void temporal_rs_PlainDateTime_days_in_year(void) { ctx_v8_rust_stub_trap("temporal_rs_PlainDateTime_days_in_year"); }
    void temporal_rs_PlainDateTime_destroy(void) { ctx_v8_rust_stub_trap("temporal_rs_PlainDateTime_destroy"); }
    void temporal_rs_PlainDateTime_epoch_ms_for_utc(void) { ctx_v8_rust_stub_trap("temporal_rs_PlainDateTime_epoch_ms_for_utc"); }
    void temporal_rs_PlainDateTime_equals(void) { ctx_v8_rust_stub_trap("temporal_rs_PlainDateTime_equals"); }
    void temporal_rs_PlainDateTime_era(void) { ctx_v8_rust_stub_trap("temporal_rs_PlainDateTime_era"); }
    void temporal_rs_PlainDateTime_era_year(void) { ctx_v8_rust_stub_trap("temporal_rs_PlainDateTime_era_year"); }
    void temporal_rs_PlainDateTime_from_parsed(void) { ctx_v8_rust_stub_trap("temporal_rs_PlainDateTime_from_parsed"); }
    void temporal_rs_PlainDateTime_from_partial(void) { ctx_v8_rust_stub_trap("temporal_rs_PlainDateTime_from_partial"); }
    void temporal_rs_PlainDateTime_hour(void) { ctx_v8_rust_stub_trap("temporal_rs_PlainDateTime_hour"); }
    void temporal_rs_PlainDateTime_in_leap_year(void) { ctx_v8_rust_stub_trap("temporal_rs_PlainDateTime_in_leap_year"); }
    void temporal_rs_PlainDateTime_microsecond(void) { ctx_v8_rust_stub_trap("temporal_rs_PlainDateTime_microsecond"); }
    void temporal_rs_PlainDateTime_millisecond(void) { ctx_v8_rust_stub_trap("temporal_rs_PlainDateTime_millisecond"); }
    void temporal_rs_PlainDateTime_minute(void) { ctx_v8_rust_stub_trap("temporal_rs_PlainDateTime_minute"); }
    void temporal_rs_PlainDateTime_month(void) { ctx_v8_rust_stub_trap("temporal_rs_PlainDateTime_month"); }
    void temporal_rs_PlainDateTime_month_code(void) { ctx_v8_rust_stub_trap("temporal_rs_PlainDateTime_month_code"); }
    void temporal_rs_PlainDateTime_months_in_year(void) { ctx_v8_rust_stub_trap("temporal_rs_PlainDateTime_months_in_year"); }
    void temporal_rs_PlainDateTime_nanosecond(void) { ctx_v8_rust_stub_trap("temporal_rs_PlainDateTime_nanosecond"); }
    void temporal_rs_PlainDateTime_round(void) { ctx_v8_rust_stub_trap("temporal_rs_PlainDateTime_round"); }
    void temporal_rs_PlainDateTime_second(void) { ctx_v8_rust_stub_trap("temporal_rs_PlainDateTime_second"); }
    void temporal_rs_PlainDateTime_since(void) { ctx_v8_rust_stub_trap("temporal_rs_PlainDateTime_since"); }
    void temporal_rs_PlainDateTime_subtract(void) { ctx_v8_rust_stub_trap("temporal_rs_PlainDateTime_subtract"); }
    void temporal_rs_PlainDateTime_to_ixdtf_string(void) { ctx_v8_rust_stub_trap("temporal_rs_PlainDateTime_to_ixdtf_string"); }
    void temporal_rs_PlainDateTime_to_plain_date(void) { ctx_v8_rust_stub_trap("temporal_rs_PlainDateTime_to_plain_date"); }
    void temporal_rs_PlainDateTime_to_plain_time(void) { ctx_v8_rust_stub_trap("temporal_rs_PlainDateTime_to_plain_time"); }
    void temporal_rs_PlainDateTime_to_zoned_date_time_with_provider(void) { ctx_v8_rust_stub_trap("temporal_rs_PlainDateTime_to_zoned_date_time_with_provider"); }
    void temporal_rs_PlainDateTime_try_new(void) { ctx_v8_rust_stub_trap("temporal_rs_PlainDateTime_try_new"); }
    void temporal_rs_PlainDateTime_until(void) { ctx_v8_rust_stub_trap("temporal_rs_PlainDateTime_until"); }
    void temporal_rs_PlainDateTime_week_of_year(void) { ctx_v8_rust_stub_trap("temporal_rs_PlainDateTime_week_of_year"); }
    void temporal_rs_PlainDateTime_with(void) { ctx_v8_rust_stub_trap("temporal_rs_PlainDateTime_with"); }
    void temporal_rs_PlainDateTime_with_calendar(void) { ctx_v8_rust_stub_trap("temporal_rs_PlainDateTime_with_calendar"); }
    void temporal_rs_PlainDateTime_with_time(void) { ctx_v8_rust_stub_trap("temporal_rs_PlainDateTime_with_time"); }
    void temporal_rs_PlainDateTime_year(void) { ctx_v8_rust_stub_trap("temporal_rs_PlainDateTime_year"); }
    void temporal_rs_PlainDateTime_year_of_week(void) { ctx_v8_rust_stub_trap("temporal_rs_PlainDateTime_year_of_week"); }
    void temporal_rs_PlainDate_add(void) { ctx_v8_rust_stub_trap("temporal_rs_PlainDate_add"); }
    void temporal_rs_PlainDate_calendar(void) { ctx_v8_rust_stub_trap("temporal_rs_PlainDate_calendar"); }
    void temporal_rs_PlainDate_clone(void) { ctx_v8_rust_stub_trap("temporal_rs_PlainDate_clone"); }
    void temporal_rs_PlainDate_compare(void) { ctx_v8_rust_stub_trap("temporal_rs_PlainDate_compare"); }
    void temporal_rs_PlainDate_day(void) { ctx_v8_rust_stub_trap("temporal_rs_PlainDate_day"); }
    void temporal_rs_PlainDate_day_of_week(void) { ctx_v8_rust_stub_trap("temporal_rs_PlainDate_day_of_week"); }
    void temporal_rs_PlainDate_day_of_year(void) { ctx_v8_rust_stub_trap("temporal_rs_PlainDate_day_of_year"); }
    void temporal_rs_PlainDate_days_in_month(void) { ctx_v8_rust_stub_trap("temporal_rs_PlainDate_days_in_month"); }
    void temporal_rs_PlainDate_days_in_week(void) { ctx_v8_rust_stub_trap("temporal_rs_PlainDate_days_in_week"); }
    void temporal_rs_PlainDate_days_in_year(void) { ctx_v8_rust_stub_trap("temporal_rs_PlainDate_days_in_year"); }
    void temporal_rs_PlainDate_destroy(void) { ctx_v8_rust_stub_trap("temporal_rs_PlainDate_destroy"); }
    void temporal_rs_PlainDate_epoch_ms_for_utc(void) { ctx_v8_rust_stub_trap("temporal_rs_PlainDate_epoch_ms_for_utc"); }
    void temporal_rs_PlainDate_equals(void) { ctx_v8_rust_stub_trap("temporal_rs_PlainDate_equals"); }
    void temporal_rs_PlainDate_era(void) { ctx_v8_rust_stub_trap("temporal_rs_PlainDate_era"); }
    void temporal_rs_PlainDate_era_year(void) { ctx_v8_rust_stub_trap("temporal_rs_PlainDate_era_year"); }
    void temporal_rs_PlainDate_from_parsed(void) { ctx_v8_rust_stub_trap("temporal_rs_PlainDate_from_parsed"); }
    void temporal_rs_PlainDate_from_partial(void) { ctx_v8_rust_stub_trap("temporal_rs_PlainDate_from_partial"); }
    void temporal_rs_PlainDate_in_leap_year(void) { ctx_v8_rust_stub_trap("temporal_rs_PlainDate_in_leap_year"); }
    void temporal_rs_PlainDate_month(void) { ctx_v8_rust_stub_trap("temporal_rs_PlainDate_month"); }
    void temporal_rs_PlainDate_month_code(void) { ctx_v8_rust_stub_trap("temporal_rs_PlainDate_month_code"); }
    void temporal_rs_PlainDate_months_in_year(void) { ctx_v8_rust_stub_trap("temporal_rs_PlainDate_months_in_year"); }
    void temporal_rs_PlainDate_since(void) { ctx_v8_rust_stub_trap("temporal_rs_PlainDate_since"); }
    void temporal_rs_PlainDate_subtract(void) { ctx_v8_rust_stub_trap("temporal_rs_PlainDate_subtract"); }
    void temporal_rs_PlainDate_to_ixdtf_string(void) { ctx_v8_rust_stub_trap("temporal_rs_PlainDate_to_ixdtf_string"); }
    void temporal_rs_PlainDate_to_plain_date_time(void) { ctx_v8_rust_stub_trap("temporal_rs_PlainDate_to_plain_date_time"); }
    void temporal_rs_PlainDate_to_plain_month_day(void) { ctx_v8_rust_stub_trap("temporal_rs_PlainDate_to_plain_month_day"); }
    void temporal_rs_PlainDate_to_plain_year_month(void) { ctx_v8_rust_stub_trap("temporal_rs_PlainDate_to_plain_year_month"); }
    void temporal_rs_PlainDate_to_zoned_date_time_with_provider(void) { ctx_v8_rust_stub_trap("temporal_rs_PlainDate_to_zoned_date_time_with_provider"); }
    void temporal_rs_PlainDate_try_new(void) { ctx_v8_rust_stub_trap("temporal_rs_PlainDate_try_new"); }
    void temporal_rs_PlainDate_until(void) { ctx_v8_rust_stub_trap("temporal_rs_PlainDate_until"); }
    void temporal_rs_PlainDate_week_of_year(void) { ctx_v8_rust_stub_trap("temporal_rs_PlainDate_week_of_year"); }
    void temporal_rs_PlainDate_with(void) { ctx_v8_rust_stub_trap("temporal_rs_PlainDate_with"); }
    void temporal_rs_PlainDate_with_calendar(void) { ctx_v8_rust_stub_trap("temporal_rs_PlainDate_with_calendar"); }
    void temporal_rs_PlainDate_year(void) { ctx_v8_rust_stub_trap("temporal_rs_PlainDate_year"); }
    void temporal_rs_PlainDate_year_of_week(void) { ctx_v8_rust_stub_trap("temporal_rs_PlainDate_year_of_week"); }
    void temporal_rs_PlainMonthDay_calendar(void) { ctx_v8_rust_stub_trap("temporal_rs_PlainMonthDay_calendar"); }
    void temporal_rs_PlainMonthDay_clone(void) { ctx_v8_rust_stub_trap("temporal_rs_PlainMonthDay_clone"); }
    void temporal_rs_PlainMonthDay_day(void) { ctx_v8_rust_stub_trap("temporal_rs_PlainMonthDay_day"); }
    void temporal_rs_PlainMonthDay_destroy(void) { ctx_v8_rust_stub_trap("temporal_rs_PlainMonthDay_destroy"); }
    void temporal_rs_PlainMonthDay_epoch_ms_for_utc(void) { ctx_v8_rust_stub_trap("temporal_rs_PlainMonthDay_epoch_ms_for_utc"); }
    void temporal_rs_PlainMonthDay_equals(void) { ctx_v8_rust_stub_trap("temporal_rs_PlainMonthDay_equals"); }
    void temporal_rs_PlainMonthDay_from_parsed(void) { ctx_v8_rust_stub_trap("temporal_rs_PlainMonthDay_from_parsed"); }
    void temporal_rs_PlainMonthDay_from_partial(void) { ctx_v8_rust_stub_trap("temporal_rs_PlainMonthDay_from_partial"); }
    void temporal_rs_PlainMonthDay_month_code(void) { ctx_v8_rust_stub_trap("temporal_rs_PlainMonthDay_month_code"); }
    void temporal_rs_PlainMonthDay_to_ixdtf_string(void) { ctx_v8_rust_stub_trap("temporal_rs_PlainMonthDay_to_ixdtf_string"); }
    void temporal_rs_PlainMonthDay_to_plain_date(void) { ctx_v8_rust_stub_trap("temporal_rs_PlainMonthDay_to_plain_date"); }
    void temporal_rs_PlainMonthDay_try_new_with_overflow(void) { ctx_v8_rust_stub_trap("temporal_rs_PlainMonthDay_try_new_with_overflow"); }
    void temporal_rs_PlainMonthDay_with(void) { ctx_v8_rust_stub_trap("temporal_rs_PlainMonthDay_with"); }
    void temporal_rs_PlainTime_add(void) { ctx_v8_rust_stub_trap("temporal_rs_PlainTime_add"); }
    void temporal_rs_PlainTime_clone(void) { ctx_v8_rust_stub_trap("temporal_rs_PlainTime_clone"); }
    void temporal_rs_PlainTime_compare(void) { ctx_v8_rust_stub_trap("temporal_rs_PlainTime_compare"); }
    void temporal_rs_PlainTime_destroy(void) { ctx_v8_rust_stub_trap("temporal_rs_PlainTime_destroy"); }
    void temporal_rs_PlainTime_epoch_ms_for_utc(void) { ctx_v8_rust_stub_trap("temporal_rs_PlainTime_epoch_ms_for_utc"); }
    void temporal_rs_PlainTime_equals(void) { ctx_v8_rust_stub_trap("temporal_rs_PlainTime_equals"); }
    void temporal_rs_PlainTime_from_partial(void) { ctx_v8_rust_stub_trap("temporal_rs_PlainTime_from_partial"); }
    void temporal_rs_PlainTime_from_utf16(void) { ctx_v8_rust_stub_trap("temporal_rs_PlainTime_from_utf16"); }
    void temporal_rs_PlainTime_from_utf8(void) { ctx_v8_rust_stub_trap("temporal_rs_PlainTime_from_utf8"); }
    void temporal_rs_PlainTime_hour(void) { ctx_v8_rust_stub_trap("temporal_rs_PlainTime_hour"); }
    void temporal_rs_PlainTime_microsecond(void) { ctx_v8_rust_stub_trap("temporal_rs_PlainTime_microsecond"); }
    void temporal_rs_PlainTime_millisecond(void) { ctx_v8_rust_stub_trap("temporal_rs_PlainTime_millisecond"); }
    void temporal_rs_PlainTime_minute(void) { ctx_v8_rust_stub_trap("temporal_rs_PlainTime_minute"); }
    void temporal_rs_PlainTime_nanosecond(void) { ctx_v8_rust_stub_trap("temporal_rs_PlainTime_nanosecond"); }
    void temporal_rs_PlainTime_round(void) { ctx_v8_rust_stub_trap("temporal_rs_PlainTime_round"); }
    void temporal_rs_PlainTime_second(void) { ctx_v8_rust_stub_trap("temporal_rs_PlainTime_second"); }
    void temporal_rs_PlainTime_since(void) { ctx_v8_rust_stub_trap("temporal_rs_PlainTime_since"); }
    void temporal_rs_PlainTime_subtract(void) { ctx_v8_rust_stub_trap("temporal_rs_PlainTime_subtract"); }
    void temporal_rs_PlainTime_to_ixdtf_string(void) { ctx_v8_rust_stub_trap("temporal_rs_PlainTime_to_ixdtf_string"); }
    void temporal_rs_PlainTime_try_new(void) { ctx_v8_rust_stub_trap("temporal_rs_PlainTime_try_new"); }
    void temporal_rs_PlainTime_until(void) { ctx_v8_rust_stub_trap("temporal_rs_PlainTime_until"); }
    void temporal_rs_PlainTime_with(void) { ctx_v8_rust_stub_trap("temporal_rs_PlainTime_with"); }
    void temporal_rs_PlainYearMonth_add(void) { ctx_v8_rust_stub_trap("temporal_rs_PlainYearMonth_add"); }
    void temporal_rs_PlainYearMonth_calendar(void) { ctx_v8_rust_stub_trap("temporal_rs_PlainYearMonth_calendar"); }
    void temporal_rs_PlainYearMonth_clone(void) { ctx_v8_rust_stub_trap("temporal_rs_PlainYearMonth_clone"); }
    void temporal_rs_PlainYearMonth_compare(void) { ctx_v8_rust_stub_trap("temporal_rs_PlainYearMonth_compare"); }
    void temporal_rs_PlainYearMonth_days_in_month(void) { ctx_v8_rust_stub_trap("temporal_rs_PlainYearMonth_days_in_month"); }
    void temporal_rs_PlainYearMonth_days_in_year(void) { ctx_v8_rust_stub_trap("temporal_rs_PlainYearMonth_days_in_year"); }
    void temporal_rs_PlainYearMonth_destroy(void) { ctx_v8_rust_stub_trap("temporal_rs_PlainYearMonth_destroy"); }
    void temporal_rs_PlainYearMonth_epoch_ms_for_utc(void) { ctx_v8_rust_stub_trap("temporal_rs_PlainYearMonth_epoch_ms_for_utc"); }
    void temporal_rs_PlainYearMonth_equals(void) { ctx_v8_rust_stub_trap("temporal_rs_PlainYearMonth_equals"); }
    void temporal_rs_PlainYearMonth_era(void) { ctx_v8_rust_stub_trap("temporal_rs_PlainYearMonth_era"); }
    void temporal_rs_PlainYearMonth_era_year(void) { ctx_v8_rust_stub_trap("temporal_rs_PlainYearMonth_era_year"); }
    void temporal_rs_PlainYearMonth_from_parsed(void) { ctx_v8_rust_stub_trap("temporal_rs_PlainYearMonth_from_parsed"); }
    void temporal_rs_PlainYearMonth_from_partial(void) { ctx_v8_rust_stub_trap("temporal_rs_PlainYearMonth_from_partial"); }
    void temporal_rs_PlainYearMonth_in_leap_year(void) { ctx_v8_rust_stub_trap("temporal_rs_PlainYearMonth_in_leap_year"); }
    void temporal_rs_PlainYearMonth_month(void) { ctx_v8_rust_stub_trap("temporal_rs_PlainYearMonth_month"); }
    void temporal_rs_PlainYearMonth_month_code(void) { ctx_v8_rust_stub_trap("temporal_rs_PlainYearMonth_month_code"); }
    void temporal_rs_PlainYearMonth_months_in_year(void) { ctx_v8_rust_stub_trap("temporal_rs_PlainYearMonth_months_in_year"); }
    void temporal_rs_PlainYearMonth_since(void) { ctx_v8_rust_stub_trap("temporal_rs_PlainYearMonth_since"); }
    void temporal_rs_PlainYearMonth_subtract(void) { ctx_v8_rust_stub_trap("temporal_rs_PlainYearMonth_subtract"); }
    void temporal_rs_PlainYearMonth_to_ixdtf_string(void) { ctx_v8_rust_stub_trap("temporal_rs_PlainYearMonth_to_ixdtf_string"); }
    void temporal_rs_PlainYearMonth_to_plain_date(void) { ctx_v8_rust_stub_trap("temporal_rs_PlainYearMonth_to_plain_date"); }
    void temporal_rs_PlainYearMonth_try_new_with_overflow(void) { ctx_v8_rust_stub_trap("temporal_rs_PlainYearMonth_try_new_with_overflow"); }
    void temporal_rs_PlainYearMonth_until(void) { ctx_v8_rust_stub_trap("temporal_rs_PlainYearMonth_until"); }
    void temporal_rs_PlainYearMonth_with(void) { ctx_v8_rust_stub_trap("temporal_rs_PlainYearMonth_with"); }
    void temporal_rs_PlainYearMonth_year(void) { ctx_v8_rust_stub_trap("temporal_rs_PlainYearMonth_year"); }
    void temporal_rs_Provider_destroy(void) { ctx_v8_rust_stub_trap("temporal_rs_Provider_destroy"); }
    void temporal_rs_Provider_empty(void) { ctx_v8_rust_stub_trap("temporal_rs_Provider_empty"); }
    void temporal_rs_Provider_new_zoneinfo64(void) { ctx_v8_rust_stub_trap("temporal_rs_Provider_new_zoneinfo64"); }
    void temporal_rs_TimeZone_identifier_with_provider(void) { ctx_v8_rust_stub_trap("temporal_rs_TimeZone_identifier_with_provider"); }
    void temporal_rs_TimeZone_try_from_identifier_str_with_provider(void) { ctx_v8_rust_stub_trap("temporal_rs_TimeZone_try_from_identifier_str_with_provider"); }
    void temporal_rs_TimeZone_try_from_offset_str(void) { ctx_v8_rust_stub_trap("temporal_rs_TimeZone_try_from_offset_str"); }
    void temporal_rs_TimeZone_try_from_str_with_provider(void) { ctx_v8_rust_stub_trap("temporal_rs_TimeZone_try_from_str_with_provider"); }
    void temporal_rs_TimeZone_utc_with_provider(void) { ctx_v8_rust_stub_trap("temporal_rs_TimeZone_utc_with_provider"); }
    void temporal_rs_TimeZone_zero(void) { ctx_v8_rust_stub_trap("temporal_rs_TimeZone_zero"); }
    void temporal_rs_ZonedDateTime_add_with_provider(void) { ctx_v8_rust_stub_trap("temporal_rs_ZonedDateTime_add_with_provider"); }
    void temporal_rs_ZonedDateTime_calendar(void) { ctx_v8_rust_stub_trap("temporal_rs_ZonedDateTime_calendar"); }
    void temporal_rs_ZonedDateTime_clone(void) { ctx_v8_rust_stub_trap("temporal_rs_ZonedDateTime_clone"); }
    void temporal_rs_ZonedDateTime_compare_instant(void) { ctx_v8_rust_stub_trap("temporal_rs_ZonedDateTime_compare_instant"); }
    void temporal_rs_ZonedDateTime_day(void) { ctx_v8_rust_stub_trap("temporal_rs_ZonedDateTime_day"); }
    void temporal_rs_ZonedDateTime_day_of_week(void) { ctx_v8_rust_stub_trap("temporal_rs_ZonedDateTime_day_of_week"); }
    void temporal_rs_ZonedDateTime_day_of_year(void) { ctx_v8_rust_stub_trap("temporal_rs_ZonedDateTime_day_of_year"); }
    void temporal_rs_ZonedDateTime_days_in_month(void) { ctx_v8_rust_stub_trap("temporal_rs_ZonedDateTime_days_in_month"); }
    void temporal_rs_ZonedDateTime_days_in_week(void) { ctx_v8_rust_stub_trap("temporal_rs_ZonedDateTime_days_in_week"); }
    void temporal_rs_ZonedDateTime_days_in_year(void) { ctx_v8_rust_stub_trap("temporal_rs_ZonedDateTime_days_in_year"); }
    void temporal_rs_ZonedDateTime_destroy(void) { ctx_v8_rust_stub_trap("temporal_rs_ZonedDateTime_destroy"); }
    void temporal_rs_ZonedDateTime_epoch_milliseconds(void) { ctx_v8_rust_stub_trap("temporal_rs_ZonedDateTime_epoch_milliseconds"); }
    void temporal_rs_ZonedDateTime_epoch_nanoseconds(void) { ctx_v8_rust_stub_trap("temporal_rs_ZonedDateTime_epoch_nanoseconds"); }
    void temporal_rs_ZonedDateTime_equals_with_provider(void) { ctx_v8_rust_stub_trap("temporal_rs_ZonedDateTime_equals_with_provider"); }
    void temporal_rs_ZonedDateTime_era(void) { ctx_v8_rust_stub_trap("temporal_rs_ZonedDateTime_era"); }
    void temporal_rs_ZonedDateTime_era_year(void) { ctx_v8_rust_stub_trap("temporal_rs_ZonedDateTime_era_year"); }
    void temporal_rs_ZonedDateTime_from_parsed_with_provider(void) { ctx_v8_rust_stub_trap("temporal_rs_ZonedDateTime_from_parsed_with_provider"); }
    void temporal_rs_ZonedDateTime_from_partial_with_provider(void) { ctx_v8_rust_stub_trap("temporal_rs_ZonedDateTime_from_partial_with_provider"); }
    void temporal_rs_ZonedDateTime_get_time_zone_transition_with_provider(void) { ctx_v8_rust_stub_trap("temporal_rs_ZonedDateTime_get_time_zone_transition_with_provider"); }
    void temporal_rs_ZonedDateTime_hour(void) { ctx_v8_rust_stub_trap("temporal_rs_ZonedDateTime_hour"); }
    void temporal_rs_ZonedDateTime_hours_in_day_with_provider(void) { ctx_v8_rust_stub_trap("temporal_rs_ZonedDateTime_hours_in_day_with_provider"); }
    void temporal_rs_ZonedDateTime_in_leap_year(void) { ctx_v8_rust_stub_trap("temporal_rs_ZonedDateTime_in_leap_year"); }
    void temporal_rs_ZonedDateTime_microsecond(void) { ctx_v8_rust_stub_trap("temporal_rs_ZonedDateTime_microsecond"); }
    void temporal_rs_ZonedDateTime_millisecond(void) { ctx_v8_rust_stub_trap("temporal_rs_ZonedDateTime_millisecond"); }
    void temporal_rs_ZonedDateTime_minute(void) { ctx_v8_rust_stub_trap("temporal_rs_ZonedDateTime_minute"); }
    void temporal_rs_ZonedDateTime_month(void) { ctx_v8_rust_stub_trap("temporal_rs_ZonedDateTime_month"); }
    void temporal_rs_ZonedDateTime_month_code(void) { ctx_v8_rust_stub_trap("temporal_rs_ZonedDateTime_month_code"); }
    void temporal_rs_ZonedDateTime_months_in_year(void) { ctx_v8_rust_stub_trap("temporal_rs_ZonedDateTime_months_in_year"); }
    void temporal_rs_ZonedDateTime_nanosecond(void) { ctx_v8_rust_stub_trap("temporal_rs_ZonedDateTime_nanosecond"); }
    void temporal_rs_ZonedDateTime_offset(void) { ctx_v8_rust_stub_trap("temporal_rs_ZonedDateTime_offset"); }
    void temporal_rs_ZonedDateTime_offset_nanoseconds(void) { ctx_v8_rust_stub_trap("temporal_rs_ZonedDateTime_offset_nanoseconds"); }
    void temporal_rs_ZonedDateTime_round_with_provider(void) { ctx_v8_rust_stub_trap("temporal_rs_ZonedDateTime_round_with_provider"); }
    void temporal_rs_ZonedDateTime_second(void) { ctx_v8_rust_stub_trap("temporal_rs_ZonedDateTime_second"); }
    void temporal_rs_ZonedDateTime_since_with_provider(void) { ctx_v8_rust_stub_trap("temporal_rs_ZonedDateTime_since_with_provider"); }
    void temporal_rs_ZonedDateTime_start_of_day_with_provider(void) { ctx_v8_rust_stub_trap("temporal_rs_ZonedDateTime_start_of_day_with_provider"); }
    void temporal_rs_ZonedDateTime_subtract_with_provider(void) { ctx_v8_rust_stub_trap("temporal_rs_ZonedDateTime_subtract_with_provider"); }
    void temporal_rs_ZonedDateTime_timezone(void) { ctx_v8_rust_stub_trap("temporal_rs_ZonedDateTime_timezone"); }
    void temporal_rs_ZonedDateTime_to_instant(void) { ctx_v8_rust_stub_trap("temporal_rs_ZonedDateTime_to_instant"); }
    void temporal_rs_ZonedDateTime_to_ixdtf_string_with_provider(void) { ctx_v8_rust_stub_trap("temporal_rs_ZonedDateTime_to_ixdtf_string_with_provider"); }
    void temporal_rs_ZonedDateTime_to_plain_date(void) { ctx_v8_rust_stub_trap("temporal_rs_ZonedDateTime_to_plain_date"); }
    void temporal_rs_ZonedDateTime_to_plain_datetime(void) { ctx_v8_rust_stub_trap("temporal_rs_ZonedDateTime_to_plain_datetime"); }
    void temporal_rs_ZonedDateTime_to_plain_time(void) { ctx_v8_rust_stub_trap("temporal_rs_ZonedDateTime_to_plain_time"); }
    void temporal_rs_ZonedDateTime_try_new_with_provider(void) { ctx_v8_rust_stub_trap("temporal_rs_ZonedDateTime_try_new_with_provider"); }
    void temporal_rs_ZonedDateTime_until_with_provider(void) { ctx_v8_rust_stub_trap("temporal_rs_ZonedDateTime_until_with_provider"); }
    void temporal_rs_ZonedDateTime_week_of_year(void) { ctx_v8_rust_stub_trap("temporal_rs_ZonedDateTime_week_of_year"); }
    void temporal_rs_ZonedDateTime_with_calendar(void) { ctx_v8_rust_stub_trap("temporal_rs_ZonedDateTime_with_calendar"); }
    void temporal_rs_ZonedDateTime_with_plain_time_and_provider(void) { ctx_v8_rust_stub_trap("temporal_rs_ZonedDateTime_with_plain_time_and_provider"); }
    void temporal_rs_ZonedDateTime_with_timezone_with_provider(void) { ctx_v8_rust_stub_trap("temporal_rs_ZonedDateTime_with_timezone_with_provider"); }
    void temporal_rs_ZonedDateTime_with_with_provider(void) { ctx_v8_rust_stub_trap("temporal_rs_ZonedDateTime_with_with_provider"); }
    void temporal_rs_ZonedDateTime_year(void) { ctx_v8_rust_stub_trap("temporal_rs_ZonedDateTime_year"); }
    void temporal_rs_ZonedDateTime_year_of_week(void) { ctx_v8_rust_stub_trap("temporal_rs_ZonedDateTime_year_of_week"); }

}  // extern "C"
