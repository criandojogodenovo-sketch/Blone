// Fase 5.1: per-corner UV tests (seams, OBJ round-trip, glTF TEXCOORD_0,
// BLONEPRJ v4, splitEdge interpolation, snapshot preservation).
// Contracts pinned here are documented in HalfEdgeMesh.h, core/io/*.h and
// core/project/Project.h.
#include "test_util.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "../core/io/GltfLoader.h"
#include "../core/io/MeshBuilder.h"
#include "../core/io/ObjExporter.h"
#include "../core/io/ObjLoader.h"
#include "../core/project/Project.h"
#include "../core/scene/Scene.h"

#ifndef BLONE_TEST_DATA_DIR
#define BLONE_TEST_DATA_DIR "data"
#endif

namespace blone::test {

using blone::ElemId;
using blone::HalfEdgeMesh;
using blone::Scene;
using blone::SceneObject;
using blone::Vec2;
using blone::Vec3;
using blone::io::MeshBuilder;
using blone::io::RawMeshData;
namespace project = blone::project;

namespace {

constexpr float kEps = 1e-6f; // round-trip tolerance (missão F5.1)

bool sameUv(const Vec2& a, const Vec2& b, float eps) {
    return std::fabs(a.x - b.x) <= eps && std::fabs(a.y - b.y) <= eps;
}

std::optional<HalfEdgeMesh> buildObjText(const std::string& text) {
    std::istringstream in(text);
    const std::optional<RawMeshData> raw = blone::io::ObjLoader::parseFile(in);
    if (!raw) return std::nullopt;
    return MeshBuilder::build(*raw);
}

// Two quads sharing the geometric edge v1-v4 with DIFFERENT uvs per face —
// the UV seam of the mission contract. `shareVertices` controls whether the
// second quad reuses the SAME vertex ElemIds (the strong seam case) or uses
// duplicated positions (the importer-produced case).
struct SeamMesh {
    HalfEdgeMesh mesh;
    ElemId fA = 0, fB = 0;
    ElemId v1 = 0, v2 = 0, v3 = 0, v4 = 0, v5 = 0, v6 = 0;
};

SeamMesh buildSeamMesh(bool shareVertices) {
    SeamMesh s;
    s.v1 = s.mesh.addVertex(Vec3{0.0f, 0.0f, 0.0f});
    s.v2 = s.mesh.addVertex(Vec3{1.0f, 0.0f, 0.0f});
    s.v3 = s.mesh.addVertex(Vec3{1.0f, 1.0f, 0.0f});
    s.v4 = s.mesh.addVertex(Vec3{0.0f, 1.0f, 0.0f});
    s.v5 = s.mesh.addVertex(Vec3{-1.0f, 0.0f, 0.0f});
    s.v6 = s.mesh.addVertex(Vec3{-1.0f, 1.0f, 0.0f});

    // Face A (front quad): uvs (0,0)(1,0)(1,1)(0,1).
    s.fA = s.mesh.addFace({s.v1, s.v2, s.v3, s.v4},
                          {Vec2{0.0f, 0.0f}, Vec2{1.0f, 0.0f},
                           Vec2{1.0f, 1.0f}, Vec2{0.0f, 1.0f}});
    // Face B (left quad, CCW from +Z): shares v1 and v4 with A but carries
    // its own uv square — the seam.
    if (shareVertices) {
        s.fB = s.mesh.addFace({s.v1, s.v4, s.v6, s.v5},
                              {Vec2{0.25f, 0.75f}, Vec2{0.75f, 0.75f},
                               Vec2{0.75f, 1.0f}, Vec2{0.25f, 1.0f}});
    } else {
        const ElemId b1 = s.mesh.addVertex(Vec3{0.0f, 0.0f, 0.0f});
        const ElemId b4 = s.mesh.addVertex(Vec3{0.0f, 1.0f, 0.0f});
        s.fB = s.mesh.addFace({b1, b4, s.v6, s.v5},
                              {Vec2{0.25f, 0.75f}, Vec2{0.75f, 0.75f},
                               Vec2{0.75f, 1.0f}, Vec2{0.25f, 1.0f}});
    }
    s.mesh.relinkBoundaryLoops();
    return s;
}

// First face-side half-edge of a face whose origin is `v` (corner carrier).
ElemId cornerHe(const HalfEdgeMesh& m, ElemId face, ElemId v) {
    for (const ElemId h : m.faceHalfEdges(face)) {
        if (m.heg(h).origin == v) return h;
    }
    return kNull;
}

} // namespace

// ---------------------------------------------------------------- 1 -------
void testUvSeamManualSharedVertex() {
    const SeamMesh s = buildSeamMesh(true);
    REQUIRE(s.fA != kNull && s.fB != kNull);
    CHECK(s.mesh.validate());

    // The SHARED geometric vertex v1 carries TWO different uvs — one per
    // face-side half-edge. Impossible with per-vertex storage; this is the
    // core Fase 5.1 contract.
    const ElemId heA = cornerHe(s.mesh, s.fA, s.v1);
    const ElemId heB = cornerHe(s.mesh, s.fB, s.v1);
    REQUIRE(heA != kNull && heB != kNull);
    CHECK(sameUv(s.mesh.uv(heA), Vec2{0.0f, 0.0f}, kEps));
    CHECK(sameUv(s.mesh.uv(heB), Vec2{0.25f, 0.75f}, kEps));
    // Same for the other seam vertex v4.
    const ElemId heA4 = cornerHe(s.mesh, s.fA, s.v4);
    const ElemId heB4 = cornerHe(s.mesh, s.fB, s.v4);
    REQUIRE(heA4 != kNull && heB4 != kNull);
    CHECK(sameUv(s.mesh.uv(heA4), Vec2{0.0f, 1.0f}, kEps));
    CHECK(sameUv(s.mesh.uv(heB4), Vec2{0.75f, 0.75f}, kEps));
    // Face B is manifold-glued to face A (shared edge reused, not duplicated).
    CHECK(s.mesh.aliveEdgeCount() == 7); // 4 (A) + 3 new (B shares v1-v4)
    CHECK(s.mesh.hasUvs());
    // The lazy array grew to cover every half-edge id issued so far
    // ("parallel to the half-edges", id-indexed).
    CHECK(s.mesh.uvs().size() > heA);
    CHECK(s.mesh.uvs().size() > heB);
}

// ---------------------------------------------------------------- 2 -------
void testAddFaceUvContract() {
    HalfEdgeMesh m;
    const ElemId a = m.addVertex(Vec3{0, 0, 0});
    const ElemId b = m.addVertex(Vec3{1, 0, 0});
    const ElemId c = m.addVertex(Vec3{0, 1, 0});

    // Non-parallel uv list: rejected WITHOUT mutation (fail-clean).
    const size_t facesBefore = m.aliveFaceCount();
    const size_t hesBefore = m.aliveHalfEdgeCount();
    CHECK(m.addFace({a, b, c}, {Vec2{0, 0}, Vec2{1, 0}}) == kNull);
    CHECK(m.aliveFaceCount() == facesBefore);
    CHECK(m.aliveHalfEdgeCount() == hesBefore);
    CHECK(!m.hasUvs()); // the lazy array was never grown

    // No-UV addFace: works exactly as before Fase 5.1, zero uv storage.
    const ElemId f1 = m.addFace({a, b, c});
    REQUIRE(f1 != kNull);
    CHECK(!m.hasUvs());
    CHECK(m.uvs().empty());
    const ElemId h1 = cornerHe(m, f1, a);
    REQUIRE(h1 != kNull);
    CHECK(sameUv(m.uv(h1), Vec2{0.0f, 0.0f}, 0.0f)); // default, no storage

    // UV addFace on the same mesh: array grows, corners carry data.
    const ElemId d = m.addVertex(Vec3{0, 0, 1});
    const ElemId e = m.addVertex(Vec3{1, 0, 1});
    const ElemId f2 = m.addFace({a, d, e, b},
                                 {Vec2{0.1f, 0.2f}, Vec2{0.3f, 0.4f},
                                  Vec2{0.5f, 0.6f}, Vec2{0.7f, 0.8f}});
    REQUIRE(f2 != kNull);
    CHECK(m.hasUvs());
    CHECK(!m.uvs().empty());
    CHECK(sameUv(m.uv(cornerHe(m, f2, a)), Vec2{0.1f, 0.2f}, kEps));
    CHECK(sameUv(m.uv(cornerHe(m, f2, b)), Vec2{0.7f, 0.8f}, kEps));
    // The pre-existing no-UV face answers the (0,0) default (no invented
    // data — documented behavior), and the mesh still validates.
    CHECK(sameUv(m.uv(h1), Vec2{0.0f, 0.0f}, 0.0f));
    m.relinkBoundaryLoops();
    CHECK(m.validate());
}

// ---------------------------------------------------------------- 3 -------
void testObjRoundTripUvs() {
    // Two quads sharing the POSITIONS v1/v4 through different vt indices —
    // the OBJ way of writing a UV seam. import -> export -> reimport must
    // give the SAME per-corner uvs (eps 1e-6, missão F5.1).
    const std::string obj = R"(# costura uv
v 0 0 0
v 1 0 0
v 1 1 0
v 0 1 0
v -1 0 0
v -1 1 0
vt 0 0
vt 1 0
vt 1 1
vt 0 1
vt 0.25 0.75
vt 0.75 0.75
vt 0.75 1
vt 0.25 1
f 1/1 2/2 3/3 4/4
f 1/5 4/6 6/7 5/8
)";
    const std::optional<HalfEdgeMesh> first = buildObjText(obj);
    REQUIRE(first.has_value());
    REQUIRE(first->validate());
    CHECK(first->hasUvs());
    CHECK(first->aliveFaceCount() == 2);
    // Corner expansion: the seam positions appear twice (2 tuples each).
    CHECK(first->aliveVertexCount() == 8);

    Scene scene;
    SceneObject objOut;
    objOut.name = "Costura";
    objOut.mesh = *first;
    scene.objects.push_back(std::move(objOut));
    scene.active = 0;

    const std::string path = "blone_test_uv_roundtrip.obj";
    REQUIRE(blone::io::ObjExporter::exportScene(scene, path));

    // The exported file must carry vt lines and v/vt/vn corners (Fase 5.1).
    {
        std::ifstream in(path);
        std::stringstream ss;
        ss << in.rdbuf();
        const std::string text = ss.str();
        CHECK(text.find("\nvt ") != std::string::npos);
        const size_t fpos = text.find("\nf ");
        REQUIRE(fpos != std::string::npos);
        // v/vt/vn: two '/' per corner token, NO empty field (no "//").
        // (eol search starts AFTER the '\n' at fpos.)
        const size_t eol = text.find('\n', fpos + 1);
        const std::string fline = text.substr(fpos, eol - fpos);
        CHECK(fline.size() > 2);
        CHECK(fline.find("//") == std::string::npos); // full form
        size_t slashes = 0;
        for (const char ch : fline) {
            if (ch == '/') ++slashes;
        }
        CHECK(slashes == 2 * 4); // 4 corners, v/vt/vn
    }

    const std::optional<RawMeshData> raw = blone::io::ObjLoader::load(path);
    std::remove(path.c_str());
    REQUIRE(raw.has_value());
    CHECK(raw->uvs.size() == raw->positions.size());
    const std::optional<HalfEdgeMesh> second = MeshBuilder::build(*raw);
    REQUIRE(second.has_value());
    REQUIRE(second->validate());

    // Face-by-face, corner-by-corner uv comparison (faces keep file order).
    REQUIRE(first->aliveFaceCount() == second->aliveFaceCount());
    const std::vector<ElemId> facesA = first->aliveFaceIds();
    const std::vector<ElemId> facesB = second->aliveFaceIds();
    for (size_t fi = 0; fi < facesA.size(); ++fi) {
        const std::vector<ElemId> hesA = first->faceHalfEdges(facesA[fi]);
        const std::vector<ElemId> hesB = second->faceHalfEdges(facesB[fi]);
        REQUIRE(hesA.size() == hesB.size());
        for (size_t ci = 0; ci < hesA.size(); ++ci) {
            const Vec2 ua = first->uv(hesA[ci]);
            const Vec2 ub = second->uv(hesB[ci]);
            CHECK_NEAR(ua.x, ub.x, kEps);
            CHECK_NEAR(ua.y, ub.y, kEps);
        }
    }
}

// ---------------------------------------------------------------- 4 -------
void testGltfTexcoordPerCorner() {
    // Real fixture (tests/data/cube_uv.glb): 24 corners, TEXCOORD_0, the
    // classic cube uv seam (8 geometric positions, 3 uv values per corner
    // position).
    const std::string path = std::string(BLONE_TEST_DATA_DIR) + "/cube_uv.glb";
    const std::optional<RawMeshData> raw = blone::io::GltfLoader::load(path);
    REQUIRE(raw.has_value());
    REQUIRE(raw->positions.size() == 24);
    REQUIRE(raw->uvs.size() == 24); // parallel, per corner

    const std::optional<HalfEdgeMesh> mesh = MeshBuilder::build(*raw);
    REQUIRE(mesh.has_value());
    REQUIRE(mesh->validate());
    CHECK(mesh->aliveFaceCount() == 12); // 12 triangles
    CHECK(mesh->hasUvs());

    // First triangle of the fixture: corners (0,0) (1,0) (1,1).
    {
        const std::vector<ElemId> hes = mesh->faceHalfEdges(
                mesh->aliveFaceIds()[0]);
        REQUIRE(hes.size() == 3);
        CHECK(sameUv(mesh->uv(hes[0]), Vec2{0.0f, 0.0f}, kEps));
        CHECK(sameUv(mesh->uv(hes[1]), Vec2{1.0f, 0.0f}, kEps));
        CHECK(sameUv(mesh->uv(hes[2]), Vec2{1.0f, 1.0f}, kEps));
    }

    // The seam: geometric corner (1,-1,1) appears in THREE faces with THREE
    // different uvs — front (1,0), right (0,0), bottom (1,1). Per-corner
    // storage must keep all of them.
    int distinctUvsAtCorner = 0;
    bool sawFront = false, sawRight = false, sawBottom = false;
    for (const ElemId f : mesh->aliveFaceIds()) {
        for (const ElemId h : mesh->faceHalfEdges(f)) {
            const Vec3& p = mesh->vrt(mesh->heg(h).origin).pos;
            if (std::fabs(p.x - 1.0f) > kEps ||
                std::fabs(p.y + 1.0f) > kEps ||
                std::fabs(p.z - 1.0f) > kEps) {
                continue;
            }
            const Vec2 u = mesh->uv(h);
            if (sameUv(u, Vec2{1.0f, 0.0f}, kEps)) sawFront = true;
            if (sameUv(u, Vec2{0.0f, 0.0f}, kEps)) sawRight = true;
            if (sameUv(u, Vec2{1.0f, 1.0f}, kEps)) sawBottom = true;
        }
    }
    if (sawFront) ++distinctUvsAtCorner;
    if (sawRight) ++distinctUvsAtCorner;
    if (sawBottom) ++distinctUvsAtCorner;
    CHECK(distinctUvsAtCorner == 3); // the seam survived, all three values
}

// ---------------------------------------------------------------- 5 -------
void testProjectV4RoundTrip() {
    // UV-bearing mesh -> save -> load: uvs bit-exact, version 4.
    const SeamMesh s = buildSeamMesh(true);
    REQUIRE(s.fA != kNull && s.fB != kNull);

    Scene scene;
    SceneObject obj;
    obj.name = "ComUV";
    obj.mesh = s.mesh;
    scene.objects.push_back(std::move(obj));
    scene.active = 0;

    const std::vector<uint8_t> bytes = project::save(scene);
    CHECK(!bytes.empty());
    uint32_t savedVersion = 0;
    std::memcpy(&savedVersion, bytes.data() + 8, 4);
    CHECK(savedVersion == project::kFormatVersion);
    CHECK(savedVersion == 4u);

    Scene loaded;
    REQUIRE(project::load(bytes, loaded) == project::Ok);
    REQUIRE(loaded.objects.size() == 1);
    const HalfEdgeMesh& lm = loaded.objects[0].mesh;
    CHECK(lm.validate());
    CHECK(lm.hasUvs());
    CHECK(lm.aliveFaceCount() == s.mesh.aliveFaceCount());
    // Half-edge ids are preserved verbatim by the format: compare the uv
    // of EVERY alive corner by id, bitwise.
    bool allSame = true;
    for (const auto& h : s.mesh.halfEdges()) {
        if (!h.alive) continue;
        const Vec2 got = lm.uv(h.id);
        const Vec2 want = s.mesh.uv(h.id);
        if (std::memcmp(&got, &want, sizeof(Vec2)) != 0) {
            allSame = false;
            break;
        }
    }
    CHECK(allSame);

    // UV-less mesh -> v4 with uvCount 0 -> loads with NO uv storage.
    Scene plain;
    SceneObject plainObj;
    {
        HalfEdgeMesh pm;
        const ElemId a = pm.addVertex(Vec3{0, 0, 0});
        const ElemId b = pm.addVertex(Vec3{1, 0, 0});
        const ElemId c = pm.addVertex(Vec3{0, 1, 0});
        REQUIRE(pm.addFace({a, b, c}) != kNull);
        pm.relinkBoundaryLoops();
        CHECK(pm.validate());
        plainObj.mesh = pm;
    }
    plain.objects.push_back(std::move(plainObj));
    plain.active = 0;
    const std::vector<uint8_t> plainBytes = project::save(plain);
    Scene plainLoaded;
    REQUIRE(project::load(plainBytes, plainLoaded) == project::Ok);
    CHECK(!plainLoaded.objects[0].mesh.hasUvs());
    CHECK(plainLoaded.objects[0].mesh.uvs().empty());
    CHECK(plainLoaded.objects[0].mesh.validate());
}

// ---------------------------------------------------------------- 6 -------
void testSplitEdgeUvInterpolation() {
    const SeamMesh s = buildSeamMesh(false); // 2 quads; split an A edge
    HalfEdgeMesh m = s.mesh;
    REQUIRE(m.validate());

    // Split the v1->v2 edge of face A (uvs (0,0) and (1,0) at its ends).
    const ElemId e12 = blone::test::edgeBetween(m, s.v1, s.v2);
    REQUIRE(e12 != kNull);
    ElemId mid = 0, eA = 0, eB = 0;
    REQUIRE(m.splitEdge(e12, mid, eA, eB));
    m.relinkBoundaryLoops();
    CHECK(m.validate());

    // Face A is now a 5-gon v1, m, v2, v3, v4; the mid corner carries the
    // linear interpolation (0.5, 0) and the untouched corners keep theirs.
    const std::vector<ElemId> hes = m.faceHalfEdges(s.fA);
    REQUIRE(hes.size() == 5);
    bool sawMid = false;
    for (const ElemId h : hes) {
        const ElemId v = m.heg(h).origin;
        if (v == mid) {
            sawMid = true;
            CHECK_NEAR(m.uv(h).x, 0.5f, kEps);
            CHECK_NEAR(m.uv(h).y, 0.0f, kEps);
        } else if (v == s.v1) {
            CHECK(sameUv(m.uv(h), Vec2{0.0f, 0.0f}, kEps));
        } else if (v == s.v2) {
            CHECK(sameUv(m.uv(h), Vec2{1.0f, 0.0f}, kEps));
        }
    }
    CHECK(sawMid);
    CHECK(m.hasUvs());
}

// ---------------------------------------------------------------- 7 -------
void testSnapshotPreservesUvs() {
    // captureRaw/restoreRaw (the History snapshot mechanism) carries the
    // lazy uv array verbatim — undo/redo keeps uvs without extra entries.
    const SeamMesh s = buildSeamMesh(true);
    const HalfEdgeMesh::Raw snap = s.mesh.captureRaw();
    CHECK(snap.uvs.size() == s.mesh.uvs().size());

    // Mutate the original past the snapshot, then restore.
    HalfEdgeMesh m2 = s.mesh;
    const ElemId a = m2.addVertex(Vec3{5, 5, 5});
    const ElemId b = m2.addVertex(Vec3{6, 5, 5});
    const ElemId c = m2.addVertex(Vec3{5, 6, 5});
    REQUIRE(m2.addFace({a, b, c}) != kNull); // no-UV face: array untouched
    m2.relinkBoundaryLoops();
    CHECK(m2.validate());

    m2.restoreRaw(snap);
    CHECK(m2.validate());
    CHECK(m2.hasUvs());
    bool allSame = true;
    for (const auto& h : snap.hes) {
        if (!h.alive) continue;
        const Vec2 got = m2.uv(h.id);
        const Vec2 want = s.mesh.uv(h.id);
        if (std::memcmp(&got, &want, sizeof(Vec2)) != 0) {
            allSame = false;
            break;
        }
    }
    CHECK(allSame);
    CHECK(m2.aliveFaceCount() == 2); // the extra face is gone with the restore
}

// ---------------------------------------------------------------- 8 -------
void testMeshBuilderRejectsNonFiniteUv() {
    RawMeshData raw;
    raw.positions = {Vec3{0, 0, 0}, Vec3{1, 0, 0}, Vec3{0, 1, 0}};
    raw.faceIndices = {{0, 1, 2}};
    // RawMeshData::uvs is blone::io::Vec2 (the I/O-local type).
    raw.uvs = {blone::io::Vec2{0.0f, 0.0f},
               blone::io::Vec2{std::nanf(""), 0.0f},
               blone::io::Vec2{1.0f, 1.0f}};
    std::string err;
    CHECK(!MeshBuilder::build(raw, &err).has_value());
    CHECK(err.find("uv não finita") != std::string::npos);
}

// ---------------------------------------------------------------- 9 -------
void testExporterNoUvKeepsHistoricalFormat() {
    // R2 guard: a mesh WITHOUT uv data must export byte-for-byte in the
    // historical format — no vt lines, v//vn corners, exactly as Fase 4.
    Scene scene;
    SceneObject obj;
    obj.name = "SemUV";
    {
        HalfEdgeMesh m;
        const ElemId a = m.addVertex(Vec3{0, 0, 0});
        const ElemId b = m.addVertex(Vec3{1, 0, 0});
        const ElemId c = m.addVertex(Vec3{0, 1, 0});
        const ElemId d = m.addVertex(Vec3{1, 1, 0});
        REQUIRE(m.addFace({a, b, d, c}) != kNull);
        m.relinkBoundaryLoops();
        CHECK(m.validate());
        obj.mesh = m;
    }
    scene.objects.push_back(std::move(obj));
    scene.active = 0;

    const std::string path = "blone_test_uv_nouv.obj";
    REQUIRE(blone::io::ObjExporter::exportScene(scene, path));
    {
        std::ifstream in(path);
        std::stringstream ss;
        ss << in.rdbuf();
        const std::string text = ss.str();
        CHECK(text.find("\nvt ") == std::string::npos); // no vt lines
        const size_t fpos = text.find("\nf ");
        REQUIRE(fpos != std::string::npos);
        const size_t eol = text.find('\n', fpos + 1);
        const std::string fline = text.substr(fpos, eol - fpos);
        CHECK(fline.size() > 2);
        CHECK(fline.find("//") != std::string::npos); // the v//vn form
        size_t slashes = 0;
        for (const char ch : fline) {
            if (ch == '/') ++slashes;
        }
        CHECK(slashes == 2 * 4); // 4 corners, v//vn (two '/' each)
    }
    // And it still reimports cleanly.
    const std::optional<RawMeshData> raw = blone::io::ObjLoader::load(path);
    std::remove(path.c_str());
    REQUIRE(raw.has_value());
    CHECK(raw->uvs.empty()); // absent, not placeholders
    const std::optional<HalfEdgeMesh> back = MeshBuilder::build(*raw);
    REQUIRE(back.has_value());
    CHECK(!back->hasUvs());
    CHECK(back->validate());
}

void runUvTests() {
    std::printf("== Fase 5.1: UVs por canto ==\n");
    testUvSeamManualSharedVertex();
    testAddFaceUvContract();
    testObjRoundTripUvs();
    testGltfTexcoordPerCorner();
    testProjectV4RoundTrip();
    testSplitEdgeUvInterpolation();
    testSnapshotPreservesUvs();
    testMeshBuilderRejectsNonFiniteUv();
    testExporterNoUvKeepsHistoricalFormat();
}

} // namespace blone::test
