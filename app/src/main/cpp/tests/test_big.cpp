// F4-LIMITS P8: BIG mesh tests — 50k / 200k / 500k / 1M triangles through
// the real pipeline (classify -> editable build OR read-only RenderMesh ->
// export). Files are generated at runtime (gen_mesh.hpp); nothing binary
// is committed. Sizes follow the owner-approved acceptance criteria:
//   50k tri -> editable on EVERY profile;
//   200k tri -> editable ONLY on Alto (read elsewhere);
//   500k/1M tri -> read-only without a crash, correct counts, export OK.
#include "test_util.h"

#include <cstdio>

#include "../core/history/History.h"
#include "../core/io/GeometryLimits.h"
#include "../core/io/MeshBuilder.h"
#include "../core/io/ObjExporter.h"
#include "../core/scene/SceneOps.h"
#include "tools/gen_mesh.hpp"

namespace {

using blone::History;
using blone::Scene;
using blone::SceneObject;
using blone::Vec3;
using blone::io::ImportClass;
using blone::io::LimitsProfile;
using blone::io::MeshBuilder;
using blone::io::classifyImport;
using blone::io::limitsFor;
using blone::io::rawTriangleCount;
namespace sceneops = blone::sceneops;

void testEditable50kAllProfiles() {
    blone::io::RawMeshData raw;
    blone::gen::generateTriangles(50000, raw);
    CHECK(rawTriangleCount(raw) == 50000);
    // Connected grid: vertex density ~tris/2 + O(sqrt(tris)) — far under
    // the Baixo vertex budget (40k).
    CHECK(raw.positions.size() >= 20000);
    CHECK(raw.positions.size() <= 30000);
    // Editable on every profile (P8 criterion).
    CHECK(classifyImport(raw.positions.size(), 50000,
                         limitsFor(LimitsProfile::Baixo)) == ImportClass::Editable);
    CHECK(classifyImport(raw.positions.size(), 50000,
                         limitsFor(LimitsProfile::Medio)) == ImportClass::Editable);
    CHECK(classifyImport(raw.positions.size(), 50000,
                         limitsFor(LimitsProfile::Alto)) == ImportClass::Editable);

    // Full facade import (history entry + undo) with the Baixo budget — the
    // tightest budget must still hold a 50k editable import.
    Scene s;
    History h;
    h.setBudget(limitsFor(LimitsProfile::Baixo).historyBytes);
    SceneObject obj;
    obj.name = "50k";
    obj.mesh = MeshBuilder::build(raw).value();
    REQUIRE(sceneops::addImportedObject(s, h, std::move(obj)) ==
            blone::ops::Status::OK);
    CHECK(s.objects[0].mesh.aliveFaceCount() == 50000);
    // The corner expansion of the connected grid == the grid vertex count.
    CHECK(s.objects[0].mesh.aliveVertexCount() == raw.positions.size());
    CHECK(h.undoDepth() == 1);
    CHECK(sceneops::undo(s, h) == blone::ops::Status::OK);
    CHECK(s.objects.empty());
    std::printf("  50k editável: mesh=%llu MB (histórico %llu/%llu MB)\n",
                static_cast<unsigned long long>(
                        s.objects.empty() ? 0 : 0), // (object moved out)
                static_cast<unsigned long long>(h.usedBytes() / (1024 * 1024)),
                static_cast<unsigned long long>(
                        limitsFor(LimitsProfile::Baixo).historyBytes /
                        (1024 * 1024)));
}

void test200kAltoEditableElseRead() {
    blone::io::RawMeshData raw;
    blone::gen::generateTriangles(200000, raw);
    CHECK(rawTriangleCount(raw) == 200000);
    // Alto: editable (and the Alto budget must hold the ObjAdd entry).
    CHECK(classifyImport(raw.positions.size(), 200000,
                         limitsFor(LimitsProfile::Alto)) ==
          ImportClass::Editable);
    {
        Scene s;
        History h;
        h.setBudget(limitsFor(LimitsProfile::Alto).historyBytes);
        SceneObject obj;
        obj.name = "200k";
        obj.mesh = MeshBuilder::build(raw).value();
        REQUIRE(sceneops::addImportedObject(s, h, std::move(obj)) ==
                blone::ops::Status::OK);
        CHECK(s.objects[0].mesh.aliveFaceCount() == 200000);
    }
    // Médio/Baixo: read-only.
    CHECK(classifyImport(raw.positions.size(), 200000,
                         limitsFor(LimitsProfile::Medio)) ==
          ImportClass::ReadOnly);
    CHECK(classifyImport(raw.positions.size(), 200000,
                         limitsFor(LimitsProfile::Baixo)) ==
          ImportClass::ReadOnly);
    // The read route: RenderMesh builds with exact counts (~30 B/tri).
    const blone::render::RenderMesh rm = blone::render::buildRenderMesh(raw);
    CHECK(rm.triangleCount == 200000);
    CHECK(rm.positions.size() == raw.positions.size()); // verbatim copy
    CHECK(rm.indices.size() == 600000); // 2 tris/quad fan == the raw faces
    CHECK(rm.memoryBytes() < 40000000ull); // well under the editable cost
}

void testReadOnlyBigAndExport() {
    // 500k and 1M: read-only, no crash, correct counts, export OK.
    for (const size_t tris : {size_t{500000}, size_t{1000000}}) {
        blone::io::RawMeshData raw;
        blone::gen::generateTriangles(tris, raw);
        CHECK(rawTriangleCount(raw) == tris);
        CHECK(classifyImport(raw.positions.size(), tris,
                             limitsFor(LimitsProfile::Alto)) ==
              ImportClass::ReadOnly);

        const blone::render::RenderMesh rm =
                blone::render::buildRenderMesh(raw);
        CHECK(rm.triangleCount == tris);
        CHECK(rm.positions.size() == raw.positions.size()); // verbatim copy
        CHECK(rm.indices.size() == tris * 3);
        CHECK(!rm.empty());
        CHECK(rm.memoryBytes() > 0);

        // Export through the read path (world = local, position (0,0,0)).
        Scene s;
        History h;
        SceneObject obj;
        obj.name = "grande";
        obj.editable = false;
        obj.readMesh = std::make_shared<const blone::render::RenderMesh>(rm);
        s.objects.push_back(std::move(obj));
        s.bump();
        const std::string path = "blone_test_big_" + std::to_string(tris) +
                                 ".obj";
        REQUIRE(blone::io::ObjExporter::exportScene(s, path));
        std::FILE* f = std::fopen(path.c_str(), "rb");
        REQUIRE(f != nullptr);
        std::fseek(f, 0, SEEK_END);
        const long long bytes =
                static_cast<long long>(std::ftell(f));
        std::fclose(f);
        // Every triangle emits ~3 "v" lines + 1 "f" line: size must be in a
        // sane range (crude but catches a truncated/empty export).
        CHECK(bytes > static_cast<long long>(tris) * 20);
        std::printf("  %zu tri: leitura OK (%llu MB render mesh), export OK "
                    "(%lld MB OBJ)\n",
                    tris,
                    static_cast<unsigned long long>(rm.memoryBytes() /
                                                    (1024 * 1024)),
                    bytes / (1024 * 1024));
        std::remove(path.c_str());
    }
}

} // namespace

namespace blone::test {

void runBigMeshTests() {
    std::printf("-- malhas grandes (50k/200k/500k/1M) --\n");
    testEditable50kAllProfiles();
    test200kAltoEditableElseRead();
    testReadOnlyBigAndExport();
}

} // namespace blone::test
