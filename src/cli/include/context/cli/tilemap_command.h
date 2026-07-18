// `context tilemap <verb>` — the tilemap cell-authoring CLI verbs (M8.5 a18, R-2D-003 GUI half /
// R-CLI-001 / L-33).
//
// The CLI face of the tile-painting surface: `tilemap paint <file> <cells> --layer <id>` writes a
// batch of [x, y, tileId] cell edits (tile 0 = empty, so an erase is a paint with 0), and `tilemap
// fill <file> <tile> --layer <id> --rect x,y,w,h` rect-fills — BOTH through the ONE editor/tilemap
// write-path core the tile-painting GUI's gesture-end commit runs (R-CLI-001: the GUI is sugar over
// this same path, proven by the tilemap-paint-parity ctest). The rewrite is canonical (owner) +
// sidecar-first (L-33 family plan) + atomic (R-FILE-004); `--if-match <raw-hash>` is the CAS guard
// and `--dry-run` stages without writing. The envelope reports the file, the cells changed, both
// labelled owner hashes, and every rewritten sidecar with its raw hash.

#pragma once

#include "context/editor/contract/envelope.h"

#include <map>
#include <string>

namespace context::cli
{

// Dispatch `context tilemap <verb> …`. `verb` is paint|fill; `bound` carries the positional params
// (paint: path + cells; fill: path + tile); `flags` the parsed flags (layer / rect / project /
// if-match / dry-run). Returns the R-CLI-008 result envelope (never throws for user input).
[[nodiscard]] editor::contract::Envelope run_tilemap(const std::string& verb,
                                                     const std::map<std::string, std::string>& bound,
                                                     const std::map<std::string, std::string>& flags);

} // namespace context::cli
