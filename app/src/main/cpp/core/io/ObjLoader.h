#pragma once
// core/io — Fase 4: Wavefront OBJ loader (own line-by-line parser, no libs).
//
// Supported subset (everything else is IGNORED, not an error):
//   v x y z            position (color defaults to white)
//   v x y z r g b      position + Blone vertex-color extension
//   v x y z w          standard 4th (rational) coordinate — w is ignored
//   vt u v [w]         texture coordinate (w ignored)
//   vn x y z           normal
//   f c/...            face corners: "v", "v/vt", "v//vn", "v/vt/vn"
// Unknown directives (o, g, s, usemtl, mtllib, l, p, comments, ...) are
// ignored; they never reject the file.
//
// Indices are 1-based; negative indices are relative to the end of the
// corresponding list (-1 = last defined). Index 0 or an out-of-range or
// forward reference is MALFORMED.
//
// Corner expansion: every unique (position, uv, normal) tuple becomes ONE
// vertex of the RawMeshData; normals/uvs/colors stay exactly parallel to
// positions. The color comes from the position's `v` line (default white).
// If ANY corner references vn (or vt), corners without one get the neutral
// placeholder (0,0,0) normal / (0,0) uv — documented convention that keeps
// the arrays parallel without inventing data.
//
// Failure contract (all-or-nothing): any malformed line — non-numeric
// fields, NaN/Inf values, wrong token counts, bad indices, faces with fewer
// than 3 corners — rejects the WHOLE file with std::nullopt; nothing is
// partially emitted. A syntactically valid file whose geometry later breaks
// the MeshBuilder contract (e.g. a face repeating a corner tuple, a face
// with more than maxFaceSides corners) parses fine and is rejected by
// MeshBuilder::build — the two stages compose into one clean rejection.

#include "MeshBuilder.h"

#include <iosfwd>
#include <optional>
#include <string>

namespace blone::io {

class ObjLoader {
public:
    // Opens `path` and parses it. std::nullopt on unreadable/malformed file.
    static std::optional<RawMeshData> load(const std::string& path);

    // Parses from an already-open stream (load() calls this; tests use it
    // for hand-written OBJ without touching the filesystem).
    static std::optional<RawMeshData> parseFile(std::istream& in);
};

} // namespace blone::io
