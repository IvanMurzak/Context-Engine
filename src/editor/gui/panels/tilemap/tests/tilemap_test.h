// Tiny zero-dependency test harness for the gui/panels/tilemap ctest executables (mirrors the
// sibling src/editor/gui/panels/problems/tests/problems_test.h — the repo carries no C++ test
// framework, so each test is a plain executable that CHECK()s its invariants and returns non-zero
// on any failure). Also carries the shared tilemap-document fixture the model/panel/a11y tests
// stage into a MemoryFileStore.

#pragma once

#include "context/editor/filesync/file_store.h"
#include "context/editor/filesync/sidecar.h"
#include "context/editor/gui/panels/tilemap/tilemap_paint_model.h"
#include "context/editor/serializer/canonical.h"
#include "context/editor/tilemap/tilemap_edit.h"

#include <cstdio>
#include <string>
#include <vector>

namespace tilemappaneltest
{
inline int g_failures = 0;

inline void fail(const char* file, int line, const char* expr)
{
    std::fprintf(stderr, "CHECK failed: %s  (%s:%d)\n", expr, file, line);
    ++g_failures;
}

inline constexpr const char* kOwnerPath = "tilemaps/map.tilemap.json";
inline constexpr const char* kGroundLayerId = "aaaaaaaaaaaaaa01";
inline constexpr const char* kPropsLayerId = "bbbbbbbbbbbbbb01";

// A minimal paintable tilemap: one tile-set spanning [1, 65), layer "ground" with an 8x8 chunk at
// the origin (sidecar dangling — the heal path covers it), layer "props" with a 2x2 chunk.
[[nodiscard]] inline std::string fixture_json()
{
    return R"({
      "$schema": "ctx:tilemap",
      "tileSize": [1, 1],
      "tileSets": [
        {"id": "7777777777777701", "name": "terrain", "firstTileId": 1, "tileCount": 64,
         "atlas": {"$ref": "a7c3e5f1b9d20648", "path": "atlases/terrain.atlas.json"}}
      ],
      "layers": [
        {"id": "aaaaaaaaaaaaaa01", "name": "ground", "chunks": [
          {"region": [0, 0, 8, 8], "cells": {"$sidecar": "map.ground.cells.bin", "hash": "1"}}
        ]},
        {"id": "bbbbbbbbbbbbbb01", "name": "props", "visible": false, "chunks": [
          {"region": [0, 0, 2, 2], "cells": {"$sidecar": "map.props.cells.bin", "hash": "1"}}
        ]}
      ]
    })";
}

// Stage the fixture (canonical bytes) into `fs` and open a model over it.
[[nodiscard]] inline bool
stage_and_open(context::editor::filesync::MemoryFileStore& fs,
               context::editor::gui::panels::tilemap::TilemapPaintModel& model)
{
    const context::editor::serializer::CanonicalizeResult canonical =
        context::editor::serializer::canonicalize(fixture_json());
    if (!canonical.is_json || !fs.write(kOwnerPath, canonical.bytes))
        return false;
    return model.open(fs, ".", kOwnerPath);
}

} // namespace tilemappaneltest

#define CHECK(cond)                                                                                \
    do                                                                                             \
    {                                                                                              \
        if (!(cond))                                                                               \
            tilemappaneltest::fail(__FILE__, __LINE__, #cond);                                     \
    } while (false)

#define TILEMAP_PANEL_TEST_MAIN_END() return tilemappaneltest::g_failures == 0 ? 0 : 1
