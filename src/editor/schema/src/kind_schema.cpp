// Kind-schema compilation (the vocabulary-law gate), the versioned registration set, the engine
// kinds, and the introspection projection — see kind_schema.h.

#include "context/editor/schema/kind_schema.h"

#include "context/editor/schema/json_access.h"
#include "context/editor/schema/vocabulary.h"
#include "context/editor/serializer/canonical.h"
#include "context/editor/serializer/json_parse.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <utility>
#include <vector>

namespace context::editor::schema
{

using serializer::JsonMember;
using serializer::JsonValue;

namespace
{

constexpr std::array<std::string_view, 6> kTypeNames = {"object", "array",   "string",
                                                        "number", "integer", "boolean"};

// The pinned dialect keywords a schema node may carry (kind_schema.h). Anything else is rejected
// so the dialect cannot drift per package.
constexpr std::array<std::string_view, 12> kSchemaKeywords = {
    "$id",   "version",     "type", "properties", "required",    "additionalProperties",
    "items", "description", "enum", "x-ctx-type", "x-ctx-units", "x-ctx-storage"};

[[nodiscard]] bool is_schema_keyword(std::string_view key) noexcept
{
    return std::find(kSchemaKeywords.begin(), kSchemaKeywords.end(), key) !=
               kSchemaKeywords.end() ||
           key == kKeyRef || key == kKeyUnion || key == kKeySidecar;
}

void add_problem(std::vector<std::string>& problems, const std::string& pointer,
                 std::string_view message)
{
    problems.push_back((pointer.empty() ? std::string("<root>") : pointer) + ": " +
                       std::string(message));
}

// Recursively law-check one schema node. `pointer` addresses the node inside the schema DOCUMENT.
void check_schema_node(const JsonValue& node, const std::string& pointer, bool is_root,
                       std::vector<std::string>& problems)
{
    if (node.type != JsonValue::Type::object)
    {
        add_problem(problems, pointer, "a schema node must be a JSON object");
        return;
    }

    for (const JsonMember& m : node.members)
        if (!is_schema_keyword(m.key))
            add_problem(problems, pointer + "/" + m.key, "unknown schema keyword");

    const JsonValue* type = find_member(node, "type");
    if (type != nullptr &&
        (type->type != JsonValue::Type::string ||
         std::find(kTypeNames.begin(), kTypeNames.end(), type->string_value) == kTypeNames.end()))
        add_problem(problems, pointer + "/type",
                    "type must be one of object/array/string/number/integer/boolean");

    if (const JsonValue* description = find_member(node, "description");
        description != nullptr && description->type != JsonValue::Type::string)
        add_problem(problems, pointer + "/description", "description must be a string");

    if (const JsonValue* additional = find_member(node, "additionalProperties");
        additional != nullptr && additional->type != JsonValue::Type::boolean)
        add_problem(problems, pointer + "/additionalProperties",
                    "additionalProperties must be a boolean");

    if (const JsonValue* enumeration = find_member(node, "enum"); enumeration != nullptr)
    {
        const bool strings = enumeration->type == JsonValue::Type::array &&
                             !enumeration->elements.empty() &&
                             std::all_of(enumeration->elements.begin(),
                                         enumeration->elements.end(), [](const JsonValue& e) {
                                             return e.type == JsonValue::Type::string;
                                         });
        if (!strings)
            add_problem(problems, pointer + "/enum", "enum must be a non-empty array of strings");
    }

    // --- the vocabulary LAW (R-DATA-006) --------------------------------------------------------
    const JsonValue* semantic = find_member(node, kKeySemanticType);
    if (semantic != nullptr && (semantic->type != JsonValue::Type::string ||
                                !is_semantic_type_id(semantic->string_value)))
        add_problem(problems, pointer + "/" + std::string(kKeySemanticType),
                    "unknown engine semantic type (pinned: quaternion, color, curve, gradient, "
                    "bit-flags)");

    const JsonValue* units = find_member(node, kKeyUnits);
    if (units != nullptr &&
        (units->type != JsonValue::Type::string || !is_si_unit(units->string_value)))
        add_problem(problems, pointer + "/" + std::string(kKeyUnits),
                    "the units law is SI + radians everywhere — non-SI unit declarations are "
                    "rejected (R-DATA-006)");

    if (const JsonValue* storage = find_member(node, kKeyStorage);
        storage != nullptr &&
        (storage->type != JsonValue::Type::string || !is_storage_layout(storage->string_value)))
        add_problem(problems, pointer + "/" + std::string(kKeyStorage),
                    "malformed storage layout (grammar: <base> or <base>x<lanes>, base in "
                    "f32/f64/i8..i64/u8..u64, lanes in 2/3/4/9/16)");

    const JsonValue* ref = find_member(node, kKeyRef);
    if (ref != nullptr && (ref->type != JsonValue::Type::string || ref->string_value.empty()))
        add_problem(problems, pointer + "/" + std::string(kKeyRef),
                    "x-ctx-ref must name the required target kind");

    const JsonValue* union_spec = find_member(node, kKeyUnion);
    if (union_spec != nullptr)
    {
        if (union_spec->type != JsonValue::Type::object || union_spec->members.empty())
            add_problem(problems, pointer + "/" + std::string(kKeyUnion),
                        "x-ctx-union must be a non-empty object of tag -> variant schema");
        else
            for (const JsonMember& variant : union_spec->members)
            {
                const std::string variant_pointer =
                    pointer + "/" + std::string(kKeyUnion) + "/" + variant.key;
                if (!is_union_tag(variant.key))
                    add_problem(problems, variant_pointer,
                                "union tags follow the ONE pinned convention \"<ns>:<shape>\" "
                                "(R-DATA-006 — never ad-hoc encodings)");
                check_schema_node(variant.value, variant_pointer, /*is_root=*/false, problems);
            }
    }

    const JsonValue* sidecar = find_member(node, kKeySidecar);
    if (sidecar != nullptr && (sidecar->type != JsonValue::Type::string ||
                               !is_sidecar_content_type(sidecar->string_value)))
        add_problem(problems, pointer + "/" + std::string(kKeySidecar),
                    "x-ctx-sidecar names the sidecar's logical content-type as one lowercase "
                    "identifier ([a-z][a-z0-9_-]*)");

    // A field is a ref, a union, a semantic value, or a binary-sidecar ref — never two at once.
    if ((semantic != nullptr) + (ref != nullptr) + (union_spec != nullptr) + (sidecar != nullptr) >
        1)
        add_problem(problems, pointer,
                    "x-ctx-type, x-ctx-ref, x-ctx-union, and x-ctx-sidecar are mutually exclusive "
                    "on one field");

    const JsonValue* properties = find_member(node, "properties");
    if (properties != nullptr)
    {
        if (properties->type != JsonValue::Type::object)
            add_problem(problems, pointer + "/properties", "properties must be an object");
        else
            for (const JsonMember& property : properties->members)
                check_schema_node(property.value, pointer + "/properties/" + property.key,
                                  /*is_root=*/false, problems);
    }

    if (const JsonValue* required = find_member(node, "required"); required != nullptr)
    {
        if (required->type != JsonValue::Type::array)
            add_problem(problems, pointer + "/required", "required must be an array of strings");
        else
            for (const JsonValue& name : required->elements)
                if (name.type != JsonValue::Type::string || properties == nullptr ||
                    find_member(*properties, name.string_value) == nullptr)
                    add_problem(problems, pointer + "/required",
                                "every required name must be a declared property");
    }

    if (const JsonValue* items = find_member(node, "items"); items != nullptr)
        check_schema_node(*items, pointer + "/items", /*is_root=*/false, problems);

    if (is_root)
    {
        const JsonValue* id = find_member(node, "$id");
        if (id == nullptr || id->type != JsonValue::Type::string || id->string_value.empty())
            add_problem(problems, pointer + "/$id", "the root must carry the kind id ($id)");
        const JsonValue* version = find_member(node, "version");
        if (version == nullptr || version->type != JsonValue::Type::integer ||
            version->int_value < 1)
            add_problem(problems, pointer + "/version",
                        "the root must carry an integer schema version >= 1");
        if (type == nullptr || type->string_value != "object")
            add_problem(problems, pointer + "/type", "an authored kind's root type is object");
        if (properties == nullptr || find_member(*properties, kNotesField) == nullptr)
            add_problem(problems, pointer + "/properties",
                        "every authored kind exposes the schema-blessed `notes` field (L-32)");
    }
}

// --- the engine kind schemas (the M1 scene placeholder, migrated onto this mechanism) -----------

constexpr std::string_view kSceneSchemaJson = R"({
  "$id": "ctx:scene",
  "version": 1,
  "type": "object",
  "additionalProperties": false,
  "required": ["entities"],
  "description": "An authored scene (L-32 canonical JSON; L-33 one scene per file).",
  "properties": {
    "kind": {"type": "string", "enum": ["scene"], "description": "The M1 placeholder kind marker; the $schema header is authoritative."},
    "notes": {"description": "Schema-blessed human/AI annotations — string or array of strings (L-32 bans JSON comments)."},
    "instances": {
      "type": "array",
      "description": "Scene composition (L-35): each entry instances another scene under a stable intra-file id; per-instance state lives in `overrides`.",
      "items": {
        "type": "object",
        "additionalProperties": false,
        "required": ["id", "scene"],
        "properties": {
          "id": {"type": "string", "description": "Stable intra-file id of this instance — the first segment of override id-paths addressing into its subtree (L-33/L-35)."},
          "scene": {"type": "string", "description": "Project-root-relative path of the instanced scene (becomes a typed x-ctx-ref when the asset database lands, L-34/L-36)."},
          "notes": {"description": "Schema-blessed annotations."}
        }
      }
    },
    "overrides": {
      "type": "array",
      "description": "Id-path-addressed override entries ([instanceId, ..., entityId] — L-35), innermost-out precedence (the OUTERMOST instancing scene wins). Exactly one kind per entry: `pointer`+`value` (per-field), `add` (structural add), or `remove: true` (structural remove) — enforced by the composition model.",
      "items": {
        "type": "object",
        "additionalProperties": false,
        "required": ["path"],
        "properties": {
          "path": {"type": "array", "items": {"type": "string"}, "description": "The id-path, from an instance id of THIS scene inward ($root addresses a composable sub-scene root)."},
          "pointer": {"type": "string", "description": "JSON pointer into the addressed entity (field override; paired with `value`). /id, /$schema and /version are immutable under composition (L-37)."},
          "value": {"description": "The overriding value — any JSON (paired with `pointer`)."},
          "base": {"description": "Optional divergence snapshot `context set` records with a field override (R-CLI-006 / L-35): the template value at write time. `context query --overrides diverged` flags an override whose `base` no longer matches the current template value (advisory; never auto-pruned)."},
          "add": {"type": "object", "description": "Structural override: an entity object (with its own stable id) composed under the instance subtree `path` addresses."},
          "remove": {"type": "boolean", "description": "Structural override: remove the entity (or whole instance subtree) `path` addresses."},
          "notes": {"description": "Schema-blessed annotations."}
        }
      }
    },
    "root": {
      "type": "object",
      "additionalProperties": false,
      "description": "The scene-root entity (L-35): scene-level state as singleton components. Inert by default when this scene is instanced as a sub-scene; `composable: true` opts it into the parent flatten. Bakes are derived artifacts, never authored.",
      "properties": {
        "id": {"type": "string", "description": "Optional stable intra-file id; when absent the root is addressable in override paths as `$root`."},
        "composable": {"type": "boolean", "description": "Opt-in: compose this root's components when the scene is instanced as a sub-scene (default false = inert)."},
        "components": {"type": "object", "description": "Singleton scene-level components."},
        "notes": {"description": "Schema-blessed annotations."}
      }
    },
    "entities": {
      "type": "array",
      "description": "The scene's entities — an id-keyed stable collection: an array of objects each carrying a stable intra-file `id` (L-33; the map-keyed form is forbidden, R-FILE-001).",
      "items": {
        "type": "object",
        "additionalProperties": false,
        "required": ["name", "components"],
        "properties": {
          "id": {"type": "string", "description": "Stable intra-file id: collision-resistant random lowercase hex, >= 64-bit, file-scoped, never sequential (L-33). Required for composition — an id-less entity cannot carry composed identity."},
          "name": {"type": "string"},
          "notes": {"description": "Schema-blessed annotations at the entity level."},
          "components": {
            "type": "object",
            "additionalProperties": false,
            "properties": {
              "notes": {"description": "Schema-blessed annotations at the component-map level."},
              "transform": {
                "type": "object",
                "additionalProperties": false,
                "required": ["position"],
                "properties": {
                  "notes": {"description": "Schema-blessed annotations."},
                  "position": {
                    "type": "array",
                    "items": {"type": "number"},
                    "x-ctx-units": "m",
                    "x-ctx-storage": "f32x3",
                    "description": "World position, meters (SI units law)."
                  }
                }
              },
              "camera": {
                "type": "object",
                "additionalProperties": false,
                "required": ["fov", "near", "far"],
                "properties": {
                  "notes": {"description": "Schema-blessed annotations."},
                  "fov": {"type": "number", "x-ctx-units": "rad", "x-ctx-storage": "f32", "description": "Vertical field of view, RADIANS (the units law: no degrees in authored data)."},
                  "near": {"type": "number", "x-ctx-units": "m", "x-ctx-storage": "f32"},
                  "far": {"type": "number", "x-ctx-units": "m", "x-ctx-storage": "f32"}
                }
              }
            }
          }
        }
      }
    }
  }
})";

constexpr std::string_view kProjectSchemaJson = R"({
  "$id": "ctx:project",
  "version": 1,
  "type": "object",
  "additionalProperties": false,
  "required": ["engine", "name", "scene"],
  "description": "The project manifest `context new` scaffolds (R-QA-006 runnable template).",
  "properties": {
    "engine": {"type": "string", "enum": ["context"]},
    "name": {"type": "string"},
    "scene": {"type": "string", "description": "Project-relative path of the startup scene (becomes a typed x-ctx-ref when the asset database lands, L-34/L-36)."},
    "notes": {"description": "Schema-blessed human/AI annotations — string or array of strings (L-32 bans JSON comments)."}
  }
})";

// The tilemap content kind (R-2D-003, M2 wave 4): the 2D backbone. Layers of chunked cell regions
// whose heavy per-cell tile-id grids live in binary SIDECARS (L-33 — the x-ctx-sidecar day-one
// consumer), painted from atlas tile-sets referenced by x-ctx-ref. Id-keyed child collections are
// arrays of objects carrying a stable `id` member (L-33 / R-FILE-001 — never map-keyed). CLI/file
// authoring works from M2; the painting GUI trails at M8.5 (L-55). Chunk cell-grid semantics
// (the ~1 MB split-nudge, id-uniqueness) live in src/editor/kinds/tilemap.h.
constexpr std::string_view kTilemapSchemaJson = R"({
  "$id": "ctx:tilemap",
  "version": 1,
  "type": "object",
  "additionalProperties": false,
  "required": ["tileSize", "tileSets", "layers"],
  "description": "A 2D tilemap (R-2D-003, L-32/L-33): back-to-front layers of chunked cell regions whose heavy per-cell tile-id grids live in binary sidecars, painted from atlas tile-sets.",
  "properties": {
    "notes": {"description": "Schema-blessed human/AI annotations — string or array of strings (L-32 bans JSON comments)."},
    "tileSize": {"type": "array", "items": {"type": "number"}, "x-ctx-units": "m", "x-ctx-storage": "f32x2", "description": "World size of one cell, meters (SI units law): [width, height]."},
    "grid": {
      "type": "object",
      "additionalProperties": false,
      "description": "Optional overall grid extent, in CELLS (dimensionless counts).",
      "properties": {
        "notes": {"description": "Schema-blessed annotations."},
        "width": {"type": "integer", "x-ctx-units": "1", "x-ctx-storage": "u32", "description": "Grid width in cells."},
        "height": {"type": "integer", "x-ctx-units": "1", "x-ctx-storage": "u32", "description": "Grid height in cells."}
      }
    },
    "tileSets": {
      "type": "array",
      "description": "The atlases tiles are painted from — an id-keyed stable collection (L-33 / R-FILE-001: array of objects, never a map).",
      "items": {
        "type": "object",
        "additionalProperties": false,
        "required": ["id", "atlas", "firstTileId"],
        "properties": {
          "notes": {"description": "Schema-blessed annotations."},
          "id": {"type": "string", "description": "Stable intra-file id: collision-resistant random lowercase hex, >= 64-bit, file-scoped, never sequential (L-33)."},
          "name": {"type": "string", "description": "Human-readable tile-set name."},
          "atlas": {"x-ctx-ref": "ctx:atlas", "description": "Typed reference to the atlas asset (L-34 dual form); target-kind enforced through the asset-db meta lookup once an atlas kind lands (L-36 / R-DATA-006)."},
          "firstTileId": {"type": "integer", "x-ctx-units": "1", "x-ctx-storage": "u32", "description": "Global tile-id of this set's first tile; per-set id ranges never overlap (Tiled-style offset)."},
          "tileCount": {"type": "integer", "x-ctx-units": "1", "x-ctx-storage": "u32", "description": "Tiles in the atlas; the set spans global ids [firstTileId, firstTileId + tileCount)."}
        }
      }
    },
    "layers": {
      "type": "array",
      "description": "Draw layers, back-to-front — an id-keyed stable collection (L-33 / R-FILE-001). Each layer's cells are chunked; each chunk's packed tile-id grid lives in a binary sidecar.",
      "items": {
        "type": "object",
        "additionalProperties": false,
        "required": ["id", "name", "chunks"],
        "properties": {
          "notes": {"description": "Schema-blessed annotations."},
          "id": {"type": "string", "description": "Stable intra-file id (L-33)."},
          "name": {"type": "string"},
          "opacity": {"type": "number", "x-ctx-units": "1", "x-ctx-storage": "f32", "description": "Layer opacity in [0, 1] (dimensionless)."},
          "visible": {"type": "boolean", "description": "Whether the layer is drawn (default true)."},
          "chunks": {
            "type": "array",
            "description": "Cell-grid regions. The heavy per-cell tile-id data lives OUT of the JSON in a binary sidecar (L-33), sized under the ~1 MB split-nudge ceiling.",
            "items": {
              "type": "object",
              "additionalProperties": false,
              "required": ["region", "cells"],
              "properties": {
                "notes": {"description": "Schema-blessed annotations."},
                "region": {"type": "array", "items": {"type": "integer"}, "x-ctx-units": "1", "x-ctx-storage": "i32x4", "description": "Chunk bounds in CELL coordinates: [x, y, width, height] (dimensionless)."},
                "cells": {"x-ctx-sidecar": "tilemap-cells", "description": "Binary sidecar of the packed per-cell tile ids (width*height u32 values), sized under the split-nudge ceiling (L-33)."}
              }
            }
          }
        }
      }
    }
  }
})";

// The string-table content kind (R-I18N-001, M2 wave 4): locale variants, fallback chains, and
// ICU/CLDR plural rules. Downstream systems (UI, dialogue) bind string KEYS from day one. Locales,
// keys, and per-locale values are all id-keyed stable collections (arrays keyed by `locale` / `key`
// / `locale` — L-33 / R-FILE-001, never maps); the CLDR plural form set uses the FIXED category
// vocabulary (zero/one/two/few/many/other) as declared properties. A value is EXACTLY ONE of `text`
// or `plural` — the dialect has no oneOf, so that single-choice rule (and fallback-cycle / plural
// completeness / id-uniqueness) is enforced by the semantic validator in src/editor/kinds/string_table.h.
constexpr std::string_view kStringTableSchemaJson = R"({
  "$id": "ctx:string-table",
  "version": 1,
  "type": "object",
  "additionalProperties": false,
  "required": ["sourceLocale", "locales", "keys"],
  "description": "A localization string table (R-I18N-001, L-32): locale variants, fallback chains, and ICU/CLDR plural rules. Downstream systems bind string KEYS from day one.",
  "properties": {
    "notes": {"description": "Schema-blessed human/AI annotations — string or array of strings (L-32 bans JSON comments)."},
    "sourceLocale": {"type": "string", "description": "BCP-47 tag of the authoritative source locale (the implicit final fallback for every key)."},
    "locales": {
      "type": "array",
      "description": "Declared locales + their fallback chains — an id-keyed stable collection (L-33 / R-FILE-001: array keyed by `locale`, never a map).",
      "items": {
        "type": "object",
        "additionalProperties": false,
        "required": ["locale"],
        "properties": {
          "notes": {"description": "Schema-blessed annotations."},
          "locale": {"type": "string", "description": "BCP-47 locale tag (e.g. \"en\", \"pt-BR\") — the stable key."},
          "name": {"type": "string", "description": "Human-readable locale name."},
          "fallback": {"type": "array", "items": {"type": "string"}, "description": "Ordered fallback chain: locale tags tried, in order, when a key has no value for this locale."}
        }
      }
    },
    "keys": {
      "type": "array",
      "description": "The string entries — an id-keyed stable collection (L-33 / R-FILE-001: array keyed by `key`). Downstream systems bind these keys.",
      "items": {
        "type": "object",
        "additionalProperties": false,
        "required": ["key", "values"],
        "properties": {
          "notes": {"description": "Schema-blessed annotations."},
          "key": {"type": "string", "description": "The stable string key downstream systems bind (the message id)."},
          "values": {
            "type": "array",
            "description": "Per-locale translations — an id-keyed stable collection keyed by `locale`. A value is EXACTLY ONE of `text` (simple) or `plural` (CLDR categories).",
            "items": {
              "type": "object",
              "additionalProperties": false,
              "required": ["locale"],
              "properties": {
                "notes": {"description": "Schema-blessed annotations."},
                "locale": {"type": "string", "description": "The locale this translation targets."},
                "text": {"type": "string", "description": "The simple (non-plural) translation."},
                "plural": {
                  "type": "object",
                  "additionalProperties": false,
                  "description": "CLDR plural forms; `other` is the required catch-all. The engine selects a category by locale + count (R-I18N-001 ICU-style rules).",
                  "properties": {
                    "notes": {"description": "Schema-blessed annotations."},
                    "zero": {"type": "string"},
                    "one": {"type": "string"},
                    "two": {"type": "string"},
                    "few": {"type": "string"},
                    "many": {"type": "string"},
                    "other": {"type": "string"}
                  }
                }
              }
            }
          }
        }
      }
    }
  }
})";

// The replay artifact content kind (R-QA-005 / L-54, M3 entry): RuntimeKernel's own serialization of
// a recorded headless run (R-FILE-009 — never an authored project file), published as a versioned
// kind so `context describe` introspects its schema like the authored kinds (R-CLI-005). The $schema
// + version header block (L-32) is read separately; the fields below are the artifact body.
constexpr std::string_view kReplaySchemaJson = R"({
  "$id": "ctx:replay",
  "version": 1,
  "type": "object",
  "additionalProperties": false,
  "required": ["seed", "tickCount", "deterministic", "inputStream"],
  "description": "A versioned, recorded headless run (R-QA-005, L-54): seed + input stream + tick count + engine/protocol versions + a content-hash manifest of the project inputs + (deterministic mode) the expected per-tick root-hash trace. Replay verifies the manifest before running (drift is reported, never silent divergence) and reports the first-divergence tick; non-deterministic replay is labeled best-effort.",
  "properties": {
    "notes": {"description": "Schema-blessed human/AI annotations — string or array of strings (L-32 bans JSON comments)."},
    "seed": {"type": "integer", "description": "The deterministic PRNG seed the run started from (splitmix64)."},
    "tickCount": {"type": "integer", "description": "The number of fixed ticks the run advanced (R-SIM-002)."},
    "scenario": {"type": "string", "description": "The named headless scenario the session ran (the built-in default is `demo`)."},
    "engineVersion": {"type": "string", "description": "The engine version that recorded the artifact (semver)."},
    "protocolMajor": {"type": "integer", "description": "The contract protocol major the artifact was recorded under (frozen at 1 by the M3 contract freeze, R-CLI-004)."},
    "deterministic": {"type": "boolean", "description": "Whether the run is deterministically reproducible; a non-deterministic artifact carries no expected trace and replays best-effort."},
    "contentManifest": {
      "type": "array",
      "description": "The project inputs the run ran against + their canonical content hashes at record time. Replay verifies these BEFORE running so a replay against drifted content is reported as drift, never a silent divergence.",
      "items": {
        "type": "object",
        "additionalProperties": false,
        "required": ["path", "hash"],
        "properties": {
          "path": {"type": "string", "description": "Project-root-relative path of the input file."},
          "hash": {"type": "integer", "description": "The serializer canonical-content hash (R-FILE-001) of the file's bytes at record time."}
        }
      }
    },
    "inputStream": {
      "type": "array",
      "description": "The recorded per-tick input: synthetic input events + mapped action activations, timestamped to the tick they applied at. Replay feeds these back at the same ticks (R-QA-005 record/replay).",
      "items": {
        "type": "object",
        "additionalProperties": false,
        "required": ["tick"],
        "properties": {
          "tick": {"type": "integer", "description": "The sim tick this input applied at."},
          "events": {
            "type": "array",
            "description": "Raw synthetic input events (the low-level layer).",
            "items": {
              "type": "object",
              "additionalProperties": false,
              "required": ["device", "code", "value"],
              "properties": {
                "device": {"type": "string", "description": "The input device (e.g. `key`, `mouse`)."},
                "code": {"type": "string", "description": "The code on the device (e.g. `W`)."},
                "value": {"type": "integer", "description": "The integer value of the event."}
              }
            }
          },
          "actions": {
            "type": "array",
            "description": "Mapped action activations (the gameplay/UI layer).",
            "items": {
              "type": "object",
              "additionalProperties": false,
              "required": ["action", "phase", "value"],
              "properties": {
                "action": {"type": "string", "description": "The mapped action name (e.g. `move_x`, `ui_submit`)."},
                "phase": {"type": "string", "enum": ["started", "performed", "canceled"], "description": "The action lifecycle phase."},
                "value": {"type": "integer", "description": "The integer value of the activation."}
              }
            }
          }
        }
      }
    },
    "expectedHashTrace": {
      "type": "array",
      "description": "The expected per-tick canonical root-hash trace (index i == tick i), present only in deterministic mode. Replay compares each tick's actual root against this and reports the first divergent tick.",
      "items": {"type": "integer"}
    }
  }
})";

constexpr std::string_view kAnimGraphSchemaJson = R"({
  "$id": "ctx:anim-graph",
  "version": 1,
  "type": "object",
  "additionalProperties": false,
  "required": ["initial", "states"],
  "description": "An authored animation state-machine / transition graph (R-SYS-008): named states, each playing one DCC-imported clip, connected by transitions gated on an integer control parameter. Canonical JSON (L-32) evaluated deterministically by the animation package (src/packages/animation/) — no in-engine clip authoring (R-ASSET-001). Referential integrity (the initial state + every transition target + parameter references resolve) is checked by the kind's semantic analyzer beyond this shape.",
  "properties": {
    "notes": {"description": "Schema-blessed human/AI annotations — string or array of strings (L-32 bans JSON comments)."},
    "initial": {"type": "string", "description": "The id of the state the graph starts in (must name a declared state)."},
    "states": {
      "type": "array",
      "description": "The graph's states. Each plays one clip and lists its outgoing parameter-gated transitions, checked in order (first satisfied wins).",
      "items": {
        "type": "object",
        "additionalProperties": false,
        "required": ["id", "clip"],
        "properties": {
          "id": {"type": "string", "description": "Stable state id, unique within the graph."},
          "clip": {"type": "string", "description": "The DCC-imported clip this state plays."},
          "transitions": {
            "type": "array",
            "description": "Outgoing transitions, evaluated in authored order; the first whose condition holds fires.",
            "items": {
              "type": "object",
              "additionalProperties": false,
              "required": ["to", "op", "threshold"],
              "properties": {
                "to": {"type": "string", "description": "Target state id (must name a declared state)."},
                "op": {"type": "string", "enum": ["ge", "gt", "le", "lt"], "description": "The control-parameter comparison: >=, >, <=, <."},
                "threshold": {"type": "number", "description": "The control-parameter threshold the comparison is against."},
                "duration": {"type": "number", "description": "The cross-fade duration in seconds when this transition fires (>= 0; 0 is instant)."}
              }
            }
          }
        }
      }
    }
  }
})";

// The audio-bus content kind (R-SYS-006 / L-46, M6 P6): a mixing-bus graph the PRESENTATION-observer
// audio package (src/packages/audio/) consumes. Buses are an id-keyed stable collection (L-33 /
// R-FILE-001: array of objects, never a map), each with a linear gain and an optional `parent` bus
// forming the mix tree. Bus-id uniqueness, parent resolution, and parent-acyclicity are REFERENTIAL
// rules the schema shape cannot express — they live in src/editor/kinds/audio_bus.h. Authored audio is
// off the deterministic sim path (R-SIM-001), so the mix-graph is pure presentation data.
constexpr std::string_view kAudioBusSchemaJson = R"({
  "$id": "ctx:audio-bus",
  "version": 1,
  "type": "object",
  "additionalProperties": false,
  "required": ["buses"],
  "description": "An authored audio mixing-bus graph (R-SYS-006, L-32/L-46): named buses with a linear gain and an optional parent bus, forming the mix tree the presentation-observer audio package routes sound events through. Bus-id uniqueness, parent resolution, and parent-acyclicity are checked by the kind's semantic analyzer beyond this shape.",
  "properties": {
    "notes": {"description": "Schema-blessed human/AI annotations — string or array of strings (L-32 bans JSON comments)."},
    "buses": {
      "type": "array",
      "description": "The mixing buses — an id-keyed stable collection (L-33 / R-FILE-001: array keyed by `id`, never a map). Each bus scales the sound routed to it and folds into its parent bus.",
      "items": {
        "type": "object",
        "additionalProperties": false,
        "required": ["id", "gain"],
        "properties": {
          "notes": {"description": "Schema-blessed annotations."},
          "id": {"type": "string", "description": "Stable intra-file bus id, unique within the graph (the routing key audio events name)."},
          "name": {"type": "string", "description": "Human-readable bus name."},
          "gain": {"type": "number", "x-ctx-units": "1", "x-ctx-storage": "f32", "description": "Linear gain multiplier applied to this bus (dimensionless; >= 0, 1.0 is unity)."},
          "parent": {"type": "string", "description": "Optional id of the bus this bus folds into; absent means a top-level (master) bus. Must name a declared bus and form no cycle (checked by the kind semantics)."}
        }
      }
    }
  }
})";

// The audio-event content kind (R-SYS-006 / L-46, M6 P6): one authored sound event the audio package
// triggers as a presentation observer. It names a DCC-imported clip (R-ASSET-001 — no in-engine audio
// authoring), routes to a bus, carries a linear gain, and an OPTIONAL 3D spatialization block
// (min/max attenuation distances). The attenuation-range consistency (maxDistance > minDistance,
// both >= 0) is a REFERENTIAL rule the schema shape cannot express — it lives in
// src/editor/kinds/audio_event.h. Off the deterministic sim path (R-SIM-001).
constexpr std::string_view kAudioEventSchemaJson = R"({
  "$id": "ctx:audio-event",
  "version": 1,
  "type": "object",
  "additionalProperties": false,
  "required": ["clip", "bus"],
  "description": "An authored sound event (R-SYS-006, L-32/L-46): a DCC-imported clip routed to a mixing bus with a linear gain and an optional 3D spatialization block. Triggered by the presentation-observer audio package (src/packages/audio/) off the deterministic sim path (R-SIM-001). The attenuation-range consistency is checked by the kind's semantic analyzer beyond this shape.",
  "properties": {
    "notes": {"description": "Schema-blessed human/AI annotations — string or array of strings (L-32 bans JSON comments)."},
    "clip": {"type": "string", "description": "The DCC-imported audio clip this event plays (R-ASSET-001 — no in-engine audio authoring)."},
    "bus": {"type": "string", "description": "The id of the mixing bus (in an authored ctx:audio-bus graph) this event routes to."},
    "gain": {"type": "number", "x-ctx-units": "1", "x-ctx-storage": "f32", "description": "Linear gain applied when this event plays (dimensionless; >= 0, 1.0 is unity)."},
    "loop": {"type": "boolean", "description": "Whether the event loops until explicitly stopped (default false)."},
    "spatial": {
      "type": "object",
      "additionalProperties": false,
      "description": "Optional 3D spatialization: when present the event is positioned in the world and attenuated by listener distance. Absent means a non-spatial (2D/UI) sound at full gain.",
      "properties": {
        "notes": {"description": "Schema-blessed annotations."},
        "minDistance": {"type": "number", "x-ctx-units": "m", "x-ctx-storage": "f32", "description": "Distance, meters (SI units law), within which the event plays at full `gain` (no attenuation)."},
        "maxDistance": {"type": "number", "x-ctx-units": "m", "x-ctx-storage": "f32", "description": "Distance, meters (SI units law), beyond which the event is fully attenuated to silence; must exceed minDistance (checked by the kind semantics)."}
      }
    }
  }
})";

[[nodiscard]] KindSchema compile_engine_schema(std::string_view schema_json)
{
    std::vector<std::string> problems;
    std::optional<KindSchema> compiled = compile_kind_schema(schema_json, problems);
    if (!compiled.has_value())
    {
        // An engine schema that violates its own vocabulary law is a programmer error caught by
        // the schema tests; fail loudly rather than publish a lawless schema.
        for (const std::string& p : problems)
            std::fprintf(stderr, "engine kind schema invalid: %s\n", p.c_str());
        std::abort();
    }
    return std::move(*compiled);
}

// --- introspection projection ---------------------------------------------------------------------

void set_member(JsonValue& object, std::string_view key, JsonValue value)
{
    object.type = JsonValue::Type::object;
    object.members.push_back(JsonMember{std::string(key), std::move(value)});
}

[[nodiscard]] JsonValue make_string(std::string_view s)
{
    JsonValue v;
    v.type = JsonValue::Type::string;
    v.string_value = std::string(s);
    return v;
}

// Flatten one schema node into per-field index entries (DATA-space pointers: "/<prop>" for
// properties, "<base>/[]" for array items, "<base>/(<tag>)" for union variants).
void collect_fields(const JsonValue& node, const std::string& data_pointer, JsonValue& fields)
{
    if (const JsonValue* properties = find_member(node, "properties"); properties != nullptr)
        for (const JsonMember& property : properties->members)
        {
            const std::string child_pointer = data_pointer + "/" + property.key;
            const JsonValue* union_spec = find_member(property.value, kKeyUnion);
            JsonValue entry;
            set_member(entry, "pointer", make_string(child_pointer));
            if (property.key == kNotesField)
                set_member(entry, "type", make_string("notes")); // the blessed annotation field
            else if (const JsonValue* type = find_member(property.value, "type"); type != nullptr)
                set_member(entry, "type", make_string(type->string_value));
            if (const JsonValue* semantic = find_member(property.value, kKeySemanticType);
                semantic != nullptr)
                set_member(entry, "semantic", make_string(semantic->string_value));
            if (const JsonValue* units = find_member(property.value, kKeyUnits); units != nullptr)
                set_member(entry, "units", make_string(units->string_value));
            if (const JsonValue* storage = find_member(property.value, kKeyStorage);
                storage != nullptr)
                set_member(entry, "storage", make_string(storage->string_value));
            if (const JsonValue* ref = find_member(property.value, kKeyRef); ref != nullptr)
                set_member(entry, "ref", make_string(ref->string_value));
            if (const JsonValue* sidecar = find_member(property.value, kKeySidecar);
                sidecar != nullptr)
                set_member(entry, "sidecar", make_string(sidecar->string_value));
            if (union_spec != nullptr)
            {
                JsonValue tags;
                tags.type = JsonValue::Type::array;
                for (const JsonMember& variant : union_spec->members)
                    tags.elements.push_back(make_string(variant.key));
                set_member(entry, "unionTags", std::move(tags));
            }
            if (const JsonValue* description = find_member(property.value, "description");
                description != nullptr)
                set_member(entry, "description", make_string(description->string_value));
            fields.elements.push_back(std::move(entry));

            collect_fields(property.value, child_pointer, fields);
            if (union_spec != nullptr)
                for (const JsonMember& variant : union_spec->members)
                    collect_fields(variant.value, child_pointer + "/(" + variant.key + ")",
                                   fields);
        }
    if (const JsonValue* items = find_member(node, "items"); items != nullptr)
        collect_fields(*items, data_pointer + "/[]", fields);
}

} // namespace

std::optional<KindSchema> compile_kind_schema(std::string_view schema_json,
                                              std::vector<std::string>& problems)
{
    serializer::ParseResult parsed = serializer::parse_json(schema_json);
    if (!parsed.ok)
    {
        for (const serializer::Diagnostic& d : parsed.diagnostics)
            add_problem(problems, "", d.code + ": " + d.message);
        return std::nullopt;
    }

    check_schema_node(parsed.root, "", /*is_root=*/true, problems);
    if (!problems.empty())
        return std::nullopt;

    KindSchema schema;
    schema.id = find_member(parsed.root, "$id")->string_value;
    schema.version = find_member(parsed.root, "version")->int_value;
    if (!serializer::serialize_canonical(parsed.root, schema.canonical_doc))
    {
        add_problem(problems, "", "schema document is not canonically serializable");
        return std::nullopt;
    }
    schema.doc = std::move(parsed.root);
    return schema;
}

void SchemaSet::add(KindSchema schema)
{
    for (KindSchema& existing : schemas_)
        if (existing.id == schema.id && existing.version == schema.version)
        {
            existing = std::move(schema);
            return;
        }
    schemas_.push_back(std::move(schema));
}

const KindSchema* SchemaSet::find(std::string_view id, std::int64_t version) const noexcept
{
    for (const KindSchema& s : schemas_)
        if (s.id == id && s.version == version)
            return &s;
    return nullptr;
}

const KindSchema* SchemaSet::latest(std::string_view id) const noexcept
{
    const KindSchema* best = nullptr;
    for (const KindSchema& s : schemas_)
        if (s.id == id && (best == nullptr || s.version > best->version))
            best = &s;
    return best;
}

const SchemaSet& engine_schemas()
{
    static const SchemaSet set = [] {
        SchemaSet s;
        s.add(compile_engine_schema(kSceneSchemaJson));
        s.add(compile_engine_schema(kProjectSchemaJson));
        s.add(compile_engine_schema(kTilemapSchemaJson));
        s.add(compile_engine_schema(kStringTableSchemaJson));
        s.add(compile_engine_schema(kReplaySchemaJson));
        s.add(compile_engine_schema(kAnimGraphSchemaJson));
        s.add(compile_engine_schema(kAudioBusSchemaJson));
        s.add(compile_engine_schema(kAudioEventSchemaJson));
        return s;
    }();
    return set;
}

std::string introspection_json(const KindSchema& schema)
{
    JsonValue entry;
    set_member(entry, "id", make_string(schema.id));
    JsonValue version;
    version.type = JsonValue::Type::integer;
    version.int_value = schema.version;
    set_member(entry, "version", std::move(version));

    JsonValue fields;
    fields.type = JsonValue::Type::array;
    collect_fields(schema.doc, "", fields);
    set_member(entry, "fields", std::move(fields));

    // The full published schema document rides along, so `describe` consumers get the authoritative
    // versioned JSON Schema (R-DATA-006) — the fields index above is a derived convenience view.
    set_member(entry, "schema", schema.doc);

    std::string out;
    if (!serializer::serialize_canonical(entry, out))
        out = "{}"; // unreachable: introspection trees carry no non-finite numbers
    return out;
}

} // namespace context::editor::schema
