#pragma once
// core/io — Fase 4: Wavefront OBJ exporter (READ-ONLY over the scene).
//
// exportScene() receives a SNAPSHOT (the JNI layer deep-copies the live
// scene via captureRaw/restoreRaw before any worker runs) and never
// mutates anything — it is a pure writer.
//
// Emitted content:
//   o <name>          one per object, name sanitized to ASCII [A-Za-z0-9_.-]
//                     (spaces -> '_', empty -> "Objeto")
//   v x y z r g b     world-space positions (position offset applied; the
//                     marked line below is the single extension point when
//                     SceneObject ever gains rotation/scale) with the
//                     vertex paint color; floats use %.9g so a float32
//                     round-trips bit-exactly through any correct parser.
//   vt u v            Fase 5.1: texture coordinates, ONE scene-wide dedup
//                     space (same rule as v/vn; %.9g bit-exact). Emitted
//                     only when at least one EDITABLE mesh carries uv
//                     data; read-only render meshes hold no uvs and an
//                     uv-less mesh keeps emitting plain v//vn corners.
//   vn x y z          SMOOTH area-weighted vertex normals: Newell's method
//                     accumulated NON-normalized per face and added to
//                     every vertex of that face, normalized once at the
//                     end. One vn per unique position.
//   f v/vt/vn         faces (the mesh is polygonal — quads stay quads).
//                     Fase 5.1: corners of uv-bearing meshes emit the full
//                     v/vt/vn form (the corner's uv comes from the
//                     face-side half-edge, so seams round-trip); meshes
//                     without uv data keep the historical v//vn form.
//                     Corners go through the tuple dedup map below so the
//                     same (position, uv, normal) tuple dedups scene-wide.
//
// Scene-wide dedup: positions and corner tuples share ONE index space for
// the whole scene (unordered_map keyed by exact float values). Consequence
// (documented limitation): coincident positions of DIFFERENT objects merge
// into one v/vn pair, and an OBJ import rebuilds a single mesh — OBJ is a
// single-mesh format.
//
// Failure contract: any file-write error returns false and removes the
// partial file. The scene is never touched.

#include "../scene/Scene.h"

#include <string>

namespace blone::io {

class ObjExporter {
public:
    // Writes the whole scene as one OBJ file. Returns false on any I/O
    // error (the scene argument is only read).
    static bool exportScene(const Scene& scene, const std::string& path);
};

} // namespace blone::io
