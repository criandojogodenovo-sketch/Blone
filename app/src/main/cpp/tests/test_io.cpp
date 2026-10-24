// Fase 4: I/O tests (OBJ import/export, glTF/GLB import, MeshBuilder).
// Contracts pinned here are documented in core/io/*.h.
#include "test_util.h"

#include <sys/stat.h> // mkdir (D5 subpath fixture; host-only suite)
#include <unistd.h>   // rmdir

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "../core/history/History.h"
#include "../core/io/GeometryLimits.h"
#include "../core/io/GltfLoader.h"
#include "../core/io/MeshBuilder.h"
#include "../core/io/ObjExporter.h"
#include "../core/io/ObjLoader.h"
#include "../core/mesh/Primitives.h"
#include "../core/scene/SceneOps.h"

#ifndef BLONE_TEST_DATA_DIR
#define BLONE_TEST_DATA_DIR "data"
#endif

namespace {

using blone::ElemId;
using blone::HalfEdgeMesh;
using blone::History;
using blone::MeshLimits;
using blone::PrimType;
using blone::Scene;
using blone::SceneObject;
using blone::ops::Status;
using blone::Vec3;
using blone::io::MeshBuilder;
using blone::io::RawMeshData;
using blone::io::Vec2;
namespace sceneops = blone::sceneops;

// parse + build: the full import pipeline for in-memory OBJ content.
std::optional<HalfEdgeMesh> buildObjText(const std::string& text) {
    std::istringstream in(text);
    const std::optional<RawMeshData> raw = blone::io::ObjLoader::parseFile(in);
    if (!raw) return std::nullopt;
    return MeshBuilder::build(*raw);
}

std::optional<RawMeshData> parseObjText(const std::string& text) {
    std::istringstream in(text);
    return blone::io::ObjLoader::parseFile(in);
}

// ---------------------------------------------------------------- 1 -------
void testObjRoundTripPreservesColors() {
    History h;
    Scene scene;
    const int idx = sceneops::createPrimitive(scene, h, PrimType::Cube, 0, 0);
    REQUIRE(idx == 0);
    h.clear(); // the primitive creation entry is irrelevant here

    SceneObject& obj = scene.objects[0];
    obj.position = Vec3{0.25f, -0.5f, 0.75f};
    obj.name = "Cubo round trip";

    // Hand-assigned paint colors (the "pintura" of the mission).
    for (auto& v : obj.mesh.vertices()) {
        if (!v.alive) continue;
        const float t = static_cast<float>(v.id % 7) / 7.0f;
        obj.mesh.vrt(v.id).color = Vec3{0.9f - t, t, 0.5f + t * 0.25f};
    }

    const std::string path = "blone_test_roundtrip.obj";
    REQUIRE(blone::io::ObjExporter::exportScene(scene, path));

    const std::optional<RawMeshData> raw = blone::io::ObjLoader::load(path);
    REQUIRE(raw.has_value());
    CHECK(raw->positions.size() == raw->colors.size()); // parallel arrays
    CHECK(!raw->colors.empty());

    const std::optional<HalfEdgeMesh> imported = MeshBuilder::build(*raw);
    std::remove(path.c_str());
    REQUIRE(imported.has_value());
    CHECK(imported->validate());

    // V/E/F identical through the export -> import cycle.
    CHECK(imported->aliveVertexCount() == obj.mesh.aliveVertexCount());
    CHECK(imported->aliveEdgeCount() == obj.mesh.aliveEdgeCount());
    CHECK(imported->aliveFaceCount() == obj.mesh.aliveFaceCount());

    // World-space positions survived (eps 1e-4) and colors are BIT-EXACT
    // when matched by position (imported = world; source = local + offset).
    for (const auto& w : imported->vertices()) {
        if (!w.alive) continue;
        const Vec3 world = w.pos;
        bool found = false;
        for (const auto& s : obj.mesh.vertices()) {
            if (!s.alive) continue;
            const Vec3 expect{s.pos.x + obj.position.x,
                              s.pos.y + obj.position.y,
                              s.pos.z + obj.position.z};
            if (std::fabs(expect.x - world.x) > 1e-4f ||
                std::fabs(expect.y - world.y) > 1e-4f ||
                std::fabs(expect.z - world.z) > 1e-4f) {
                continue;
            }
            found = true;
            const Vec3 a = imported->vrt(w.id).color;
            const Vec3 b = obj.mesh.vrt(s.id).color;
            CHECK(std::memcmp(&a, &b, sizeof(Vec3)) == 0);
            break;
        }
        CHECK(found);
    }
}

// ---------------------------------------------------------------- 2 -------
void testObjHandWrittenVtVnAndNegatives() {
    // Quad (1..4, CCW) + triangle reusing tuples through NEGATIVE indices;
    // tri (p0,p3,p2) shares edges in the opposite direction (manifold-ok).
    const std::string obj = R"(# cubo teste
v 0 0 0
v 1 0 0
v 1 1 0
v 0 1 0
vt 0 0
vt 1 0
vt 1 1
vt 0 1
vn 0 0 1
f 1/1/1 2/2/1 3/3/1 4/4/1
f -4/-4/1 -1/-1/1 -2/-2/1
)";
    std::istringstream in(obj);
    const std::optional<RawMeshData> raw = blone::io::ObjLoader::parseFile(in);
    REQUIRE(raw.has_value());

    // Corner expansion: the tri's negative indices resolve to tuples the
    // quad already created (p0/-4, p3/-1, p2/-2 with matching vt) — so only
    // 4 unique tuples exist, all arrays parallel to them.
    CHECK(raw->positions.size() == 4);
    CHECK(raw->normals.size() == 4);
    CHECK(raw->uvs.size() == 4);
    CHECK(raw->colors.size() == 4);
    CHECK(raw->faceIndices.size() == 2);
    CHECK(raw->faceIndices[0].size() == 4);
    CHECK(raw->faceIndices[1].size() == 3);
    // Default color = white (no `v x y z r g b` lines in this file).
    for (const Vec3& c : raw->colors) {
        CHECK(c.x == 1.0f && c.y == 1.0f && c.z == 1.0f);
    }
    // The negative-index tri reuses quad tuples: corners equal quad ids.
    CHECK(raw->faceIndices[1][0] == raw->faceIndices[0][0]);
    CHECK(raw->faceIndices[1][1] == raw->faceIndices[0][3]);
    CHECK(raw->faceIndices[1][2] == raw->faceIndices[0][2]);

    const std::optional<HalfEdgeMesh> mesh = MeshBuilder::build(*raw);
    REQUIRE(mesh.has_value());
    CHECK(mesh->validate());
    CHECK(mesh->aliveFaceCount() == 2);
}

// ---------------------------------------------------------------- 3 -------
void testObjMalformedRejected() {
    // Out-of-range index: rejected at PARSE stage.
    CHECK(!parseObjText("v 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 9\n").has_value());
    // NaN position: rejected at PARSE stage.
    CHECK(!parseObjText("v nan 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3\n").has_value());
    // Inf normal: rejected at PARSE stage.
    CHECK(!parseObjText("v 0 0 0\nv 1 0 0\nv 0 1 0\nvn inf 0 0\n"
                        "f 1 2 3\n")
                .has_value());
    // Repeated vertex inside one face: parses, rejected by the BUILDER.
    {
        const std::optional<HalfEdgeMesh> m = buildObjText(
                "v 0 0 0\nv 1 0 0\nv 0 1 0\nv 1 1 0\nf 1 2 2 3\nf 4 3 2\n");
        CHECK(!m.has_value());
    }
    // Face with fewer than 3 corners: rejected.
    CHECK(!buildObjText("v 0 0 0\nv 1 0 0\nf 1 2\n").has_value());
    // Index 0: rejected (OBJ indices are 1-based or negative).
    CHECK(!buildObjText("v 0 0 0\nv 1 0 0\nv 0 1 0\nf 0 1 2\n").has_value());
    // Empty file / geometry-less file: rejected.
    CHECK(!buildObjText("# s\u00f3 um coment\u00e1rio\n").has_value());
}

// ---------------------------------------------------------------- 4 -------
void testGlbFixtureImport() {
    const std::string path =
            std::string(BLONE_TEST_DATA_DIR) + "/cube.glb";
    std::ifstream probe(path, std::ios::binary);
    REQUIRE(probe.good());

    const std::optional<RawMeshData> raw = blone::io::GltfLoader::load(path);
    REQUIRE(raw.has_value());
    // The fixture carries POSITION only: colors must be EMPTY at raw level
    // (MeshBuilder then defaults every vertex to white, "vazio = branco").
    CHECK(raw->positions.size() == 24);
    CHECK(raw->colors.empty());
    CHECK(raw->faceIndices.size() == 12);

    const std::optional<HalfEdgeMesh> mesh = MeshBuilder::build(*raw);
    REQUIRE(mesh.has_value());
    CHECK(mesh->validate());
    CHECK(mesh->aliveFaceCount() == 12);
    CHECK(mesh->aliveVertexCount() == 24);
    // Default white paint, parallel to every vertex.
    for (const auto& v : mesh->vertices()) {
        if (!v.alive) continue;
        const Vec3 c = mesh->vrt(v.id).color;
        CHECK(c.x == 1.0f && c.y == 1.0f && c.z == 1.0f);
    }
}

// ---------------------------------------------------------------- 4b ------
// Hotfix D1: real-file robustness — the EXACT mix the owner hits on the
// device: CRLF line endings, group/material directives (o/g/usemtl/mtllib),
// comments, and NEGATIVE v and vn indices. Loaded through the FILE PATH
// (like the device worker), not an in-memory stream.
void testObjRealFileCrlfGroupsNegatives() {
    const std::string path = "blone_test_crlf.obj";
    {
        std::ofstream out(path, std::ios::binary);
        REQUIRE(out.good());
        out << "# Exportado por FerramentaReal 1.0\r\n"
               "mtllib cube_real.mtl\r\n"
               "o Cubo_Real.001\r\n"
               "g grupo_principal\r\n"
               "s off\r\n"
               "v -1 -1 1\r\n"
               "v 1 -1 1\r\n"
               "v 1 1 1\r\n"
               "v -1 1 1\r\n"
               "v -1 -1 -1\r\n"
               "v 1 -1 -1\r\n"
               "v 1 1 -1\r\n"
               "v -1 1 -1\r\n"
               "usemtl Material_Base\r\n"
               "vn 0 0 1\r\n"
               "vn 1 0 0\r\n"
               "vn 0 0 -1\r\n"
               "vn -1 0 0\r\n"
               "vn 0 1 0\r\n"
               "vn 0 -1 0\r\n"
               "f 1//1 2//1 3//1 4//1\r\n"
               "f -7//-5 -3//-5 -2//-5 -6//-5\r\n"
               "f 6//3 5//3 8//3 7//3\r\n"
               "f 5//4 1//4 4//4 8//4\r\n"
               "f 4//5 3//5 7//5 8//5\r\n"
               "f 5//6 6//6 2//6 1//6\r\n";
    }

    std::string err;
    const std::optional<RawMeshData> raw = blone::io::ObjLoader::load(path, &err);
    std::remove(path.c_str());
    REQUIRE(raw.has_value());
    CHECK(err.empty()); // success => NO error text (D1 err contract)
    // Corner expansion: unique (position, normal) tuples of the 6 quads.
    CHECK(raw->positions.size() == 24);
    CHECK(raw->normals.size() == raw->positions.size()); // parallel
    CHECK(raw->colors.size() == raw->positions.size());  // white default
    CHECK(raw->faceIndices.size() == 6);                 // 6 quad faces

    const std::optional<HalfEdgeMesh> mesh = MeshBuilder::build(*raw, &err);
    REQUIRE(mesh.has_value());
    CHECK(err.empty());
    CHECK(mesh->validate());
    CHECK(mesh->aliveVertexCount() == 24);
    CHECK(mesh->aliveFaceCount() == 6);
}

// ---------------------------------------------------------------- 4c ------
// Hotfix D1: embedded-buffer glTF (the .gltf self-contained form real tools
// write) through the FILE PATH, plus the err contract of the loader.
void testGltfEmbeddedDataUriFixture() {
    const std::string path =
            std::string(BLONE_TEST_DATA_DIR) + "/cube_embedded.gltf";
    std::ifstream probe(path, std::ios::binary);
    REQUIRE(probe.good());

    std::string err;
    const std::optional<RawMeshData> raw = blone::io::GltfLoader::load(path, &err);
    REQUIRE(raw.has_value());
    CHECK(err.empty()); // success => NO error text (D1 err contract)
    CHECK(raw->positions.size() == 8);   // indexed shared-corner cube
    CHECK(raw->colors.empty());          // no COLOR_0 => MeshBuilder whites
    CHECK(raw->faceIndices.size() == 12); // 12 triangles

    const std::optional<HalfEdgeMesh> mesh = MeshBuilder::build(*raw, &err);
    REQUIRE(mesh.has_value());
    CHECK(err.empty());
    CHECK(mesh->validate());
    CHECK(mesh->aliveVertexCount() == 8);
    CHECK(mesh->aliveFaceCount() == 12);

    // The failure side of the err contract: a truncated/glTF-foreign file
    // rejects WITH a PT reason (the device toast shows it — R8).
    const std::string badPath = "blone_test_bad.glb";
    {
        std::ofstream out(badPath, std::ios::binary);
        out << "isto não é um glTF";
    }
    std::string badErr;
    CHECK(!blone::io::GltfLoader::load(badPath, &badErr).has_value());
    CHECK(!badErr.empty()); // motivo PT presente
    std::remove(badPath.c_str());

    // OBJ side of the err contract: malformed line reports the line number.
    const std::string badObj = "blone_test_bad.obj";
    {
        std::ofstream out(badObj, std::ios::binary);
        out << "v 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 9\n";
    }
    std::string objErr;
    CHECK(!blone::io::ObjLoader::load(badObj, &objErr).has_value());
    CHECK(objErr.find("linha 4") != std::string::npos); // motivo PT + linha
    std::remove(badObj.c_str());
}

// ---------------------------------------------------------------- 4d ------
// Hotfix D5: .gltf with EXTERNAL .bin buffers — the two-step import flow.
// The device failure: SAF copies only the .gltf to the cache; the .bin
// sibling stays in Downloads and cgltf_load_buffers fails with a generic
// "buffers não carregados". Now the loader reports the missing BASE names
// (percent-decoded) so the Kotlin layer can ask for the folder that
// contains them (IMPORT_NEEDS_FILES + retryLastImport).
void writeD5Gltf(const char* path, const char* bufferUri) {
    std::ofstream out(path, std::ios::binary);
    REQUIRE(out.good());
    out << "{\n"
           "  \"asset\": {\"version\": \"2.0\"},\n"
           "  \"scene\": 0,\n"
           "  \"scenes\": [{\"nodes\": [0]}],\n"
           "  \"nodes\": [{\"mesh\": 0}],\n"
           "  \"meshes\": [{\"primitives\": [{\"attributes\": {\"POSITION\": 1},"
           " \"indices\": 0, \"mode\": 4}]}],\n"
           "  \"buffers\": [{\"uri\": \"" << bufferUri
        << "\", \"byteLength\": 168}],\n"
           "  \"bufferViews\": [\n"
           "    {\"buffer\": 0, \"byteOffset\": 0, \"byteLength\": 72,"
           " \"target\": 34963},\n"
           "    {\"buffer\": 0, \"byteOffset\": 72, \"byteLength\": 96,"
           " \"target\": 34962}],\n"
           "  \"accessors\": [\n"
           "    {\"bufferView\": 0, \"componentType\": 5123, \"count\": 36,"
           " \"type\": \"SCALAR\"},\n"
           "    {\"bufferView\": 1, \"componentType\": 5126, \"count\": 8,"
           " \"type\": \"VEC3\", \"min\": [-1, -1, -1], \"max\": [1, 1, 1]}]\n"
           "}\n";
}

void writeD5Bin(const char* path) {
    static const uint16_t idx[36] = {
            0, 1, 2, 0, 2, 3,   1, 5, 6, 1, 6, 2,   5, 4, 7, 5, 7, 6,
            4, 0, 3, 4, 3, 7,   3, 2, 6, 3, 6, 7,   4, 5, 1, 4, 1, 0};
    static const float pos[24] = {-1, -1, 1, 1, -1, 1, 1, 1, 1, -1, 1, 1,
                                  -1, -1, -1, 1, -1, -1, 1, 1, -1, -1, 1, -1};
    std::ofstream out(path, std::ios::binary);
    REQUIRE(out.good());
    out.write(reinterpret_cast<const char*>(idx), sizeof(idx));
    out.write(reinterpret_cast<const char*>(pos), sizeof(pos));
}

void testGltfExternalBuffersTwoStep() {
    const std::string doc = "blone_test_d5_model.gltf";
    const std::string bin = "cube_ext.bin";

    // (b) WITHOUT the .bin: nullopt + the EXACT base name in the list + a
    // PT reason (R8). This is the payload the IMPORT_NEEDS_FILES protocol
    // carries to Kotlin.
    writeD5Gltf(doc.c_str(), "cube_ext.bin");
    {
        std::string err;
        std::vector<std::string> missing;
        const auto raw = blone::io::GltfLoader::load(doc, &err, nullptr, &missing);
        CHECK(!raw.has_value());
        REQUIRE(missing.size() == 1);
        CHECK(missing[0] == "cube_ext.bin"); // nome exato (missão D5)
        CHECK(!err.empty());                 // motivo PT presente
    }

    // (a)+(c) the Kotlin flow copied the .bin next to the cached document
    // ("a MESMA dir de cache do .gltf") and re-fired the import: OK, the
    // missing list stays EMPTY on success and the geometry is complete.
    writeD5Bin(bin.c_str());
    {
        std::string err;
        std::vector<std::string> missing;
        const auto raw = blone::io::GltfLoader::load(doc, &err, nullptr, &missing);
        REQUIRE(raw.has_value());
        CHECK(missing.empty());
        CHECK(err.empty()); // success => NO error text (D1 err contract)
        CHECK(raw->positions.size() == 8);
        CHECK(raw->faceIndices.size() == 12);
        auto mesh = MeshBuilder::build(*raw, &err);
        REQUIRE(mesh.has_value());
        CHECK(mesh->validate());
        CHECK(mesh->aliveFaceCount() == 12);
    }

    // (d) percent-decode + subpath: uri "sub%20dir/my%20buffer.bin" — the
    // MISSING report carries the DECODED BASE name (what the user's folder
    // contains and what Kotlin must copy); cgltf decodes the uri on the
    // file path, so once the decoded file exists the import succeeds.
    const std::string doc2 = "blone_test_d5_model2.gltf";
    writeD5Gltf(doc2.c_str(), "sub%20dir/my%20buffer.bin");
    {
        std::string err;
        std::vector<std::string> missing;
        CHECK(!blone::io::GltfLoader::load(doc2, &err, nullptr, &missing)
                   .has_value());
        REQUIRE(missing.size() == 1);
        CHECK(missing[0] == "my buffer.bin"); // decodificado, só o nome-base
    }
    mkdir("sub dir", 0777);
    writeD5Bin("sub dir/my buffer.bin");
    {
        std::string err;
        std::vector<std::string> missing;
        const auto raw = blone::io::GltfLoader::load(doc2, &err, nullptr, &missing);
        REQUIRE(raw.has_value());
        CHECK(missing.empty());
        CHECK(raw->positions.size() == 8);
    }

    // GLB / data-URI documents NEVER take the missing-files path (the
    // mission: they keep working without any dialog).
    {
        std::string err;
        std::vector<std::string> missing;
        const auto raw = blone::io::GltfLoader::load(
                std::string(BLONE_TEST_DATA_DIR) + "/cube_embedded.gltf", &err,
                nullptr, &missing);
        REQUIRE(raw.has_value());
        CHECK(missing.empty());
        const auto glb = blone::io::GltfLoader::load(
                std::string(BLONE_TEST_DATA_DIR) + "/cube.glb", &err, nullptr,
                &missing);
        REQUIRE(glb.has_value());
        CHECK(missing.empty());
    }

    std::remove(doc.c_str());
    std::remove(bin.c_str());
    std::remove(doc2.c_str());
    std::remove("sub dir/my buffer.bin");
    rmdir("sub dir");
}

// ---------------------------------------------------------------- 5 -------
void testImportHistoryUndoRedo() {
    History h;
    Scene s;

    // Build an import candidate OUTSIDE the scene (as the worker does).
    RawMeshData data;
    data.positions = {Vec3{0, 0, 0}, Vec3{1, 0, 0}, Vec3{1, 1, 0},
                      Vec3{0, 1, 0}};
    data.colors = {Vec3{1, 0, 0}, Vec3{0, 1, 0}, Vec3{0, 0, 1},
                   Vec3{1, 1, 0}};
    data.faceIndices.push_back({0, 1, 2, 3});
    const std::optional<HalfEdgeMesh> mesh = MeshBuilder::build(data);
    REQUIRE(mesh.has_value());

    SceneObject obj;
    obj.name = "Importado";
    obj.mesh = std::move(*mesh);

    CHECK(sceneops::addImportedObject(s, h, std::move(obj)) == Status::OK);
    CHECK(s.objects.size() == 1);
    CHECK(s.active == 0); // hotfix D3: the drain ACTIVATES the import — the
    // properties panel reads THIS index right after the drain.
    CHECK(h.undoDepth() == 1); // ONE coherent entry for ONE import
    CHECK(h.redoDepth() == 0);

    // Undo removes the imported object entirely.
    CHECK(sceneops::undo(s, h) == Status::OK);
    CHECK(s.objects.empty());
    CHECK(h.redoDepth() == 1);

    // Redo restores it (same counts, same colors).
    CHECK(sceneops::redo(s, h) == Status::OK);
    REQUIRE(s.objects.size() == 1);
    CHECK(s.objects[0].mesh.aliveVertexCount() == 4);
    CHECK(s.objects[0].mesh.aliveFaceCount() == 1);
    const Vec3 c = s.objects[0].mesh.vrt(1).color;
    CHECK(c.x == 1.0f && c.y == 0.0f && c.z == 0.0f);
}

// ---------------------------------------------------------------- 6 -------
void testRawAboveLimitsRejected() {
    // Vertex limit: 200001 positions > MeshLimits::maxVertices.
    {
        RawMeshData data;
        data.positions.resize(blone::MeshLimits::maxVertices + 1);
        for (size_t i = 0; i < data.positions.size(); ++i) {
            const float f = static_cast<float>(i);
            data.positions[i] = Vec3{f, f * 0.5f, f * 0.25f};
        }
        data.colors.resize(data.positions.size(), Vec3{1.0f, 1.0f, 1.0f});
        data.faceIndices.push_back({0, 1, 2});
        CHECK(!MeshBuilder::build(data).has_value());
    }
    // Face side limit: 257 corners > maxFaceSides (256).
    {
        RawMeshData data;
        for (int i = 0; i < 300; ++i) {
            const float a = static_cast<float>(i) * 0.1f;
            data.positions.push_back(Vec3{a, 0.0f, 0.0f});
        }
        std::vector<int> loop;
        for (int i = 0; i < 257; ++i) loop.push_back(i);
        data.faceIndices.push_back(loop);
        CHECK(!MeshBuilder::build(data).has_value());
    }
    // Face-count limit is exercised by construction (200001 faces would be
    // slow); the parallel-array contract is pinned instead: colors shorter
    // than positions => rejected.
    {
        RawMeshData data;
        data.positions = {Vec3{0, 0, 0}, Vec3{1, 0, 0}, Vec3{0, 1, 0}};
        data.colors = {Vec3{1, 1, 1}};
        data.faceIndices.push_back({0, 1, 2});
        CHECK(!MeshBuilder::build(data).has_value());
    }
}

// ------------------------------------------------ F4-LIMITS P1 ------------
void testFormatCountAndClassify() {
    using blone::io::formatCount;
    using blone::io::limitsFor;
    using blone::io::classifyImport;
    using blone::io::ImportClass;
    using blone::io::LimitsProfile;
    // PT thousands separator.
    CHECK(formatCount(0) == "0");
    CHECK(formatCount(999) == "999");
    CHECK(formatCount(1000) == "1.000");
    CHECK(formatCount(40000) == "40.000");
    CHECK(formatCount(1000000) == "1.000.000");
    CHECK(formatCount(4000000) == "4.000.000");
    CHECK(formatCount(4000001) == "4.000.001");

    // Approved table (values pinned so a silent change is caught).
    // historyBytes sizing rule: must hold the ObjAdd entry of the biggest
    // editable import of the profile (~270 B per editable triangle).
    const auto baixo = limitsFor(LimitsProfile::Baixo);
    const auto medio = limitsFor(LimitsProfile::Medio);
    const auto alto = limitsFor(LimitsProfile::Alto);
    CHECK(baixo.editVerts == 40000 && baixo.editTris == 80000 &&
          baixo.viewTris == 500000 && baixo.historyBytes == 32ull * 1024 * 1024);
    CHECK(medio.editVerts == 75000 && medio.editTris == 150000 &&
          medio.viewTris == 1000000 && medio.historyBytes == 48ull * 1024 * 1024);
    CHECK(alto.editVerts == 150000 && alto.editTris == 300000 &&
          alto.viewTris == 2000000 && alto.historyBytes == 96ull * 1024 * 1024);

    // P8 acceptance criteria encoded in the classification:
    // 50k tri -> EDITABLE on every profile.
    CHECK(classifyImport(25000, 50000, baixo) == ImportClass::Editable);
    CHECK(classifyImport(25000, 50000, medio) == ImportClass::Editable);
    CHECK(classifyImport(25000, 50000, alto) == ImportClass::Editable);
    // 200k tri -> editable ONLY on Alto, read-only elsewhere.
    CHECK(classifyImport(100000, 200000, alto) == ImportClass::Editable);
    CHECK(classifyImport(100000, 200000, medio) == ImportClass::ReadOnly);
    CHECK(classifyImport(100000, 200000, baixo) == ImportClass::ReadOnly);
    // 500k tri -> read-only everywhere; 1M tri -> read on Médio/Alto.
    CHECK(classifyImport(250000, 500000, baixo) == ImportClass::ReadOnly);
    CHECK(classifyImport(250000, 500000, medio) == ImportClass::ReadOnly);
    CHECK(classifyImport(250000, 500000, alto) == ImportClass::ReadOnly);
    CHECK(classifyImport(500000, 1000000, medio) == ImportClass::ReadOnly);
    CHECK(classifyImport(500000, 1000000, alto) == ImportClass::ReadOnly);
    // Above the view limit / above the hard ceiling -> rejected.
    CHECK(classifyImport(500000, 1000001, medio) == ImportClass::Rejected);
    CHECK(classifyImport(500000, 1000001, baixo) == ImportClass::Rejected);
    CHECK(classifyImport(MeshLimits::maxVertices + 1, 10, alto) ==
          ImportClass::Rejected);
    // Boundary: exactly at the limits stays editable.
    CHECK(classifyImport(baixo.editVerts, baixo.editTris, baixo) ==
          ImportClass::Editable);
    // rawTriangleCount: quad + tri = 2 + 1.
    {
        RawMeshData d;
        d.positions = {Vec3{0, 0, 0}, Vec3{1, 0, 0}, Vec3{1, 1, 0},
                       Vec3{0, 1, 0}, Vec3{2, 0, 0}};
        d.faceIndices.push_back({0, 1, 2, 3});
        d.faceIndices.push_back({1, 4, 2});
        CHECK(blone::io::rawTriangleCount(d) == 3);
    }
}

void testLimitReasonsCarryNumbers() {
    using blone::io::LimitsProfile;
    const std::string edit = blone::io::reasonAboveEditLimit(
            LimitsProfile::Medio, 25000, 200000);
    CHECK(edit.find("Médio") != std::string::npos);
    CHECK(edit.find("150.000") != std::string::npos);
    CHECK(edit.find("200.000") != std::string::npos);
    CHECK(edit.find("25.000") != std::string::npos);
    const std::string view = blone::io::reasonAboveViewLimit(
            LimitsProfile::Baixo, 300000, 600000);
    CHECK(view.find("Baixo") != std::string::npos);
    CHECK(view.find("500.000") != std::string::npos);
    CHECK(view.find("600.000") != std::string::npos);
    const std::string hard =
            blone::io::reasonAboveHardCeiling(4000001, 8100000);
    CHECK(hard.find("4.000.000") != std::string::npos);
    CHECK(hard.find("8.000.000") != std::string::npos);
}

void testHardCeilingMessageCarriesNumbers() {
    // 4M+1 positions (48 MB transient) — the same contract test 6 pins, but
    // now asserting the REJECTION MESSAGE carries the numbers (R8): the
    // device dialog quotes them verbatim.
    RawMeshData data;
    data.positions.resize(MeshLimits::maxVertices + 1);
    data.faceIndices.push_back({0, 1, 2});
    std::string err;
    const auto built = MeshBuilder::build(data, &err);
    CHECK(!built.has_value());
    CHECK(err.find("4.000.000") != std::string::npos);
    CHECK(err.find("4.000.001") != std::string::npos);
}

void testObjTupleKeyBeyond21Bits() {
    // Regression (missão F4-LIMITS, P1): the corner-tuple key packed
    // (p,t,n) into 21-bit fields of one uint64. With uv/normal lists beyond
    // 2^21 (2.097.152) entries — legal now that the hard ceiling is 4M —
    // the packed fields bled into the neighbours and two DIFFERENT corners
    // deduped into one (silent geometry corruption). This file puts the two
    // colliding corners in separate triangles:
    //   face A: "f 1//1 2//1 3//1"      -> corners (p, absent, 0)
    //   face B: "f 1/2^21/1 2/2^21/1 3/2^21/1" -> corners (p, 2^21-1, 0)
    // Under the OLD packing the absent sentinel (all-ones 21-bit field)
    // equals the real uv index 2^21-1 shifted the same way: every corner of
    // B deduped into its A twin (uvs lost, 3 corners instead of 6). The
    // struct key keeps all six distinct.
    constexpr int kVtCount = (1 << 21); // exactly 2^21 vt entries
    std::ostringstream obj;
    obj << "# gerado: chave de tuplo para alem de 21 bits\n";
    obj << "v 0 0 0\nv 1 0 0\nv 2 0 0\n";
    obj << "vn 0 0 1\n";
    // uv.x encodes the entry index exactly (floats hold ints up to 2^24).
    for (int i = 0; i < kVtCount; ++i) {
        obj << "vt " << i << " 0\n";
    }
    obj << "f 1//1 2//1 3//1\n";
    obj << "f 1/" << kVtCount << "/1 2/" << kVtCount << "/1 3/" << kVtCount
        << "/1\n";
    std::istringstream in(obj.str());
    const auto raw = blone::io::ObjLoader::parseFile(in);
    REQUIRE(raw.has_value());
    CHECK(raw->positions.size() == 6); // 3 corners per face; old packing: 3
    CHECK(raw->uvs.size() == 6);       // old packing deduped to 3
    CHECK(raw->normals.size() == 6);
    REQUIRE(raw->faceIndices.size() == 2);
    REQUIRE(raw->faceIndices[0].size() == 3);
    REQUIRE(raw->faceIndices[1].size() == 3);
    for (const int c : raw->faceIndices[0]) {
        CHECK(raw->uvs[static_cast<size_t>(c)].x == 0.0f); // placeholder
    }
    for (const int c : raw->faceIndices[1]) {
        // uv = vt[2^21-1] — NOT the placeholder the old packing produced.
        CHECK(raw->uvs[static_cast<size_t>(c)].x ==
              static_cast<float>(kVtCount - 1));
        CHECK(raw->normals[static_cast<size_t>(c)].z == 1.0f);
    }
    // Builds through the full pipeline.
    const auto mesh = MeshBuilder::build(*raw);
    REQUIRE(mesh.has_value());
    CHECK(mesh->aliveVertexCount() == 6);
    CHECK(mesh->aliveFaceCount() == 2);
}

// ------------------------------------------------ F4-LIMITS P4 ------------
void testProgressAndCancel() {
    using blone::io::IoProgress;

    // IoProgress contract: monotonic clamp + cancel flag.
    {
        IoProgress p;
        CHECK(p.get() == 0);
        p.set(300);
        p.set(200); // backwards: ignored (monotonic)
        CHECK(p.get() == 300);
        p.set(500);
        CHECK(p.get() == 500);
        CHECK(!p.cancelled());
        p.requestCancel();
        CHECK(p.cancelled());
        p.reset();
        CHECK(p.get() == 0 && !p.cancelled());
    }

    // A pre-set cancel flag unwinds the parse cleanly with a PT reason.
    {
        std::istringstream in("v 0 0 0\nv 1 0 0\nv 2 0 0\nf 1 2 3\n");
        IoProgress p;
        p.requestCancel();
        std::string err;
        const auto raw = blone::io::ObjLoader::parseFile(in, &err, &p);
        CHECK(!raw.has_value());
        CHECK(err.find("cancelada") != std::string::npos);
    }

    // Cancel DURING the parse (mid-file): the flag raised by a cancel-check
    // interval boundary stops the parse — still all-or-nothing.
    {
        std::ostringstream obj;
        obj << "v 0 0 0\nv 1 0 0\nv 2 0 0\n";
        for (int i = 0; i < 30000; ++i) {
            obj << "v " << i << " 0 1\n";
        }
        obj << "f 1 2 3\n";
        std::istringstream in(obj.str());
        IoProgress p;
        std::string err;
        // The cancel lands while parsing; parseFile must reject.
        p.requestCancel();
        const auto raw = blone::io::ObjLoader::parseFile(in, &err, &p);
        CHECK(!raw.has_value());
        CHECK(err.find("cancelada") != std::string::npos);
    }

    // A clean import THROUGH the pre-count path: progress advances and the
    // file loads (the counting pass + parse pass compose).
    {
        const std::string path = "blone_test_precount.obj";
        {
            std::ofstream out(path, std::ios::binary);
            REQUIRE(out.good());
            out << "v 0 0 0\nv 1 0 0\nv 2 0 0\nv 0 1 0\nf 1 2 3\nf 1 3 4\n";
        }
        IoProgress p;
        std::string err;
        const auto raw = blone::io::ObjLoader::load(path, &err, &p);
        REQUIRE(raw.has_value());
        CHECK(raw->positions.size() == 4);
        CHECK(raw->faceIndices.size() == 2);
        CHECK(p.get() > 0); // the counting/parse passes published progress
        std::remove(path.c_str());
    }
}

} // namespace

namespace blone::test {

void runIoTests() {
    std::printf("-- io (obj/gltf, round-trip de pintura) --\n");
    testObjRoundTripPreservesColors();
    testObjHandWrittenVtVnAndNegatives();
    testObjMalformedRejected();
    testGlbFixtureImport();
    testObjRealFileCrlfGroupsNegatives();   // hotfix D1
    testGltfEmbeddedDataUriFixture();       // hotfix D1
    testGltfExternalBuffersTwoStep();       // hotfix D5
    testImportHistoryUndoRedo();
    testRawAboveLimitsRejected();
    testFormatCountAndClassify();           // F4-LIMITS P1
    testLimitReasonsCarryNumbers();         // F4-LIMITS P1
    testHardCeilingMessageCarriesNumbers(); // F4-LIMITS P1
    testObjTupleKeyBeyond21Bits();          // F4-LIMITS P1
    testProgressAndCancel();                // F4-LIMITS P4
}

} // namespace blone::test
