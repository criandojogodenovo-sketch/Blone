#include "ObjExporter.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

namespace blone::io {

namespace {

// Exact-value keys (bitwise equality — exporters write what the mesh holds;
// no epsilon merging, so a round-trip preserves the vertex set exactly).
struct PosKey {
    float x, y, z;
    bool operator==(const PosKey& o) const {
        return x == o.x && y == o.y && z == o.z;
    }
};
struct PosKeyHash {
    size_t operator()(const PosKey& k) const {
        auto mix = [](float f) {
            uint32_t u = 0;
            std::memcpy(&u, &f, sizeof(u));
            return static_cast<size_t>(u) * 0x9E3779B9u;
        };
        return mix(k.x) ^ (mix(k.y) << 1) ^ (mix(k.z) << 2);
    }
};

// Corner tuple (position index, uv index, normal index). The uv slot is the
// constant 0 placeholder until per-corner UV storage exists (Fase 5) — the
// mechanism itself is complete: identical (pos, uv, normal) tuples dedup
// through one map shared by the WHOLE scene.
struct CornerKey {
    uint32_t pos, uv, nrm;
    bool operator==(const CornerKey& o) const {
        return pos == o.pos && uv == o.uv && nrm == o.nrm;
    }
};
struct CornerKeyHash {
    size_t operator()(const CornerKey& k) const {
        return (static_cast<size_t>(k.pos) * 73856093u) ^
               (static_cast<size_t>(k.uv) * 19349663u) ^
               (static_cast<size_t>(k.nrm) * 83492791u);
    }
};

std::string sanitizeName(const std::string& raw) {
    std::string out;
    out.reserve(raw.size());
    for (const char c : raw) {
        const unsigned char u = static_cast<unsigned char>(c);
        if (c == ' ') {
            out.push_back('_'); // spaces become underscores
        } else if (u >= 0x20 && u <= 0x7E) {
            out.push_back(c); // printable ASCII kept
        } // control/non-ASCII dropped
    }
    if (out.empty()) out = "Objeto";
    return out;
}

} // namespace

bool ObjExporter::exportScene(const Scene& scene, const std::string& path) {
    // ---- pass A: unique world-space positions (scene-wide dedup) --------
    // MARK (Fase 5 extension point): when SceneObject gains rotation/scale,
    // the full transform is applied HERE — world = T * R * S * local.
    std::vector<Vec3> worldPos;
    std::vector<Vec3> colorOfPos;   // paint color of the first source vertex
    std::vector<uint32_t> ownerOfPos; // first object that used the position
    std::unordered_map<PosKey, uint32_t, PosKeyHash> posIndex;
    std::vector<std::vector<uint32_t>> posOfObject(scene.objects.size());
    std::vector<std::vector<std::vector<uint32_t>>> facesOfObject(
            scene.objects.size()); // object -> face -> position-index loop

    size_t totalFaces = 0;
    for (const SceneObject& obj : scene.objects) {
        totalFaces += obj.mesh.aliveFaceCount();
    }
    if (totalFaces == 0 || totalFaces > MeshLimits::maxFaces) return false;
    worldPos.reserve(totalFaces * 4);
    colorOfPos.reserve(totalFaces * 4);
    ownerOfPos.reserve(totalFaces * 4);

    for (size_t oi = 0; oi < scene.objects.size(); ++oi) {
        const SceneObject& obj = scene.objects[oi];
        if (obj.mesh.empty()) continue;
        std::vector<std::vector<uint32_t>>& objFaces = facesOfObject[oi];
        objFaces.reserve(obj.mesh.aliveFaceCount());
        for (const ElemId f : obj.mesh.aliveFaceIds()) {
            const std::vector<ElemId> loop = obj.mesh.faceVertices(f);
            if (loop.size() < 3) continue; // defensive; validate() forbids
            std::vector<uint32_t> faceLoop;
            faceLoop.reserve(loop.size());
            for (const ElemId v : loop) {
                const Vec3 world{obj.mesh.vrt(v).pos.x + obj.position.x,
                                 obj.mesh.vrt(v).pos.y + obj.position.y,
                                 obj.mesh.vrt(v).pos.z + obj.position.z};
                auto it = posIndex.find(PosKey{world.x, world.y, world.z});
                if (it == posIndex.end()) {
                    const uint32_t id = static_cast<uint32_t>(worldPos.size());
                    worldPos.push_back(world);
                    colorOfPos.push_back(obj.mesh.vrt(v).color);
                    ownerOfPos.push_back(static_cast<uint32_t>(oi));
                    it = posIndex.emplace(PosKey{world.x, world.y, world.z}, id)
                                 .first;
                }
                const uint32_t pid = it->second;
                faceLoop.push_back(pid);
            }
            objFaces.push_back(std::move(faceLoop));
        }
    }
    if (worldPos.empty()) return false; // nothing exportable

    // v/vn emission list: each unique position appears exactly once, under
    // its FIRST owning object (rebuild without per-corner duplicates).
    for (uint32_t pid = 0; pid < worldPos.size(); ++pid) {
        posOfObject[ownerOfPos[pid]].push_back(pid);
    }

    // ---- pass B: smooth area-weighted normals (Newell, non-normalized) --
    // The face's Newell vector (proportional to twice its area) is
    // accumulated once and added to EVERY position of that face; the sum is
    // normalized a single time at the end.
    std::vector<Vec3> accum(worldPos.size(), Vec3{0.0f, 0.0f, 0.0f});
    for (const auto& objFaces : facesOfObject) {
        for (const std::vector<uint32_t>& loop : objFaces) {
            Vec3 faceN{0.0f, 0.0f, 0.0f};
            const size_t n = loop.size();
            for (size_t i = 0; i < n; ++i) {
                const Vec3& a = worldPos[loop[i]];
                const Vec3& b = worldPos[loop[(i + 1) % n]];
                faceN.x += (a.y - b.y) * (a.z + b.z);
                faceN.y += (a.z - b.z) * (a.x + b.x);
                faceN.z += (a.x - b.x) * (a.y + b.y);
            }
            for (const uint32_t pid : loop) {
                accum[pid].x += faceN.x;
                accum[pid].y += faceN.y;
                accum[pid].z += faceN.z;
            }
        }
    }
    std::vector<Vec3> normals(worldPos.size());
    for (size_t i = 0; i < worldPos.size(); ++i) {
        const float len = std::sqrt(accum[i].x * accum[i].x +
                                    accum[i].y * accum[i].y +
                                    accum[i].z * accum[i].z);
        normals[i] = (len > 1e-12f)
                         ? Vec3{accum[i].x / len, accum[i].y / len, accum[i].z / len}
                         : Vec3{0.0f, 0.0f, 0.0f}; // degenerate: zero normal
    }

    // ---- pass C: write the file -----------------------------------------
    std::FILE* out = std::fopen(path.c_str(), "w");
    if (!out) return false;
    auto fail = [&out, &path]() {
        std::fclose(out);
        std::remove(path.c_str()); // never leave a partial file behind
        return false;
    };

    // Corner tuple dedup shared across the whole scene. Value = global loop
    // id the tuple was last used in: re-emitting the tuple in ANOTHER face
    // is normal (OBJ f lines reference indices), but a tuple repeating
    // INSIDE one loop is dropped — the same guard the future corner
    // expander will apply when UVs arrive.
    std::unordered_map<CornerKey, uint64_t, CornerKeyHash> cornerDedup;
    uint64_t loopGlobalId = 0;

    for (size_t oi = 0; oi < scene.objects.size(); ++oi) {
        if (facesOfObject[oi].empty() && posOfObject[oi].empty()) continue;
        if (std::fprintf(out, "o %s\n",
                         sanitizeName(scene.objects[oi].name).c_str()) < 0) {
            return fail();
        }
        // v + vn lines for the positions first owned by this object
        // (scene-wide dedup means later objects may reference them too).
        for (const uint32_t pid : posOfObject[oi]) {
            const Vec3& p = worldPos[pid];
            const Vec3& c = colorOfPos[pid];
            const Vec3& n = normals[pid];
            if (std::fprintf(out,
                             "v %.9g %.9g %.9g %.9g %.9g %.9g\n"
                             "vn %.9g %.9g %.9g\n",
                             static_cast<double>(p.x), static_cast<double>(p.y),
                             static_cast<double>(p.z), static_cast<double>(c.x),
                             static_cast<double>(c.y), static_cast<double>(c.z),
                             static_cast<double>(n.x), static_cast<double>(n.y),
                             static_cast<double>(n.z)) < 0) {
                return fail();
            }
        }
        for (const std::vector<uint32_t>& loop : facesOfObject[oi]) {
            if (std::fprintf(out, "f") < 0) return fail();
            ++loopGlobalId;
            size_t emitted = 0;
            for (const uint32_t pid : loop) {
                const CornerKey ck{pid, 0u, pid}; // uv slot 0 until Fase 5
                auto [it, inserted] = cornerDedup.emplace(ck, loopGlobalId);
                if (!inserted && it->second == loopGlobalId) {
                    continue; // tuple repeated inside this loop: skip
                }
                it->second = loopGlobalId;
                // OBJ is 1-based: v//vn (vn index == position index today,
                // one smooth normal per unique position).
                if (std::fprintf(out, " %u//%u", static_cast<unsigned>(pid + 1),
                                 static_cast<unsigned>(pid + 1)) < 0) {
                    return fail();
                }
                ++emitted;
            }
            if (std::fprintf(out, "\n") < 0) return fail();
            if (emitted < 3) return fail(); // cannot happen; belt and braces
        }
    }

    if (std::fflush(out) != 0) return fail();
    if (std::fclose(out) != 0) {
        std::remove(path.c_str());
        return false;
    }
    return true;
}

} // namespace blone::io
