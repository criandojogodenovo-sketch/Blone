// Fase 3 — brush tests (sculpt + paint), covering the 12 required scenarios:
// no-vertices-in-radius, vertices-in-radius, radius/strength respected,
// invalid values rejected, mesh integrity, paint region only, untouched
// colors preserved, sculpt undo/redo, paint undo/redo, cancel, persistence
// (v2) and v1 file compatibility.
//
// NOTE on scene coordinates: sceneops::createPrimitive places the cube at
// (0, 1, 0) with half extent 1 — corners are (±1, 0/2, ±1).
#include "test_util.h"

#include "../core/brush/Brush.h"
#include "../core/history/History.h"
#include "../core/mesh/Primitives.h"
#include "../core/project/Project.h"
#include "../core/scene/SceneOps.h"

#include <cmath>
#include <cstring>
#include <vector>

using namespace blone;

namespace {

// ---------------------------------------------------------------- helpers --

Scene makeCubeScene(History& h) {
    Scene s;
    const int idx = sceneops::createPrimitive(s, h, PrimType::Cube, 0, 0);
    CHECK(idx == 0);
    return s;
}

bool samePositions(const HalfEdgeMesh& a, const HalfEdgeMesh& b) {
    if (a.aliveVertexCount() != b.aliveVertexCount()) return false;
    for (const auto& v : a.vertices()) {
        if (!v.alive) continue;
        if (!b.vertexAlive(v.id)) return false;
        const Vec3& p = a.vrt(v.id).pos;
        const Vec3& q = b.vrt(v.id).pos;
        if (std::memcmp(&p, &q, sizeof(Vec3)) != 0) return false;
    }
    return true;
}

bool sameColors(const HalfEdgeMesh& a, const HalfEdgeMesh& b) {
    if (a.aliveVertexCount() != b.aliveVertexCount()) return false;
    for (const auto& v : a.vertices()) {
        if (!v.alive) continue;
        if (!b.vertexAlive(v.id)) return false;
        const Vec3& p = a.vrt(v.id).color;
        const Vec3& q = b.vrt(v.id).color;
        if (std::memcmp(&p, &q, sizeof(Vec3)) != 0) return false;
    }
    return true;
}

// Cube top corners are at y = 2 (center at y=1). Finds a corner by sign.
ElemId cubeCorner(const HalfEdgeMesh& m, float sx, float sy, float sz) {
    const float wy = (sy > 0.0f) ? 2.0f : 0.0f;
    const Vec3 expect{1.0f * sx, wy, 1.0f * sz};
    for (const auto& v : m.vertices()) {
        if (!v.alive) continue;
        if (length(v.pos - expect) < 0.5f) return v.id;
    }
    return kNull;
}

// ------------------------------------------------------------------ 1 + 2 --

void testSculptCollectAndApply() {
    // 1) NO vertices inside the radius -> nothing changes.
    {
        HalfEdgeMesh m;
        PrimitiveParams p;
        p.cx = 0; p.cy = 0; p.cz = 0; p.size = 1.0f;
        REQUIRE(buildPrimitive(PrimType::Cube, p, m));
        const HalfEdgeMesh::Raw before = m.captureRaw();

        std::vector<ElemId> affected;
        brush::collectAffected(m, Vec3{100.0f, 100.0f, 100.0f}, 0.5f, affected);
        CHECK(affected.empty());

        brush::BrushSettings s;
        s.radius = 0.5f;
        s.strength = 1.0f;
        CHECK(brush::sculptApply(m, affected, Vec3{100.0f, 100.0f, 100.0f},
                                 Vec3{0, 1, 0}, s));
        CHECK(test::sameMesh(m, test::meshFromRaw(before)));

        // Session level: a far-but-finite hit is a valid no-op stroke.
        History h;
        Scene sc = makeCubeScene(h);
        h.clear(); // drop the ObjAdd entry: we measure only the stroke
        sceneops::BrushSession b;
        brush::BrushSettings bs;
        bs.radius = 0.5f;
        bs.strength = 1.0f;
        CHECK(sceneops::beginBrushStroke(sc, h, b, sceneops::BrushTool::Sculpt, 0,
                                         Vec3{50.0f, 50.0f, 50.0f}, Vec3{0, 1, 0},
                                         bs) == ops::Status::OK);
        CHECK(b.active);
        CHECK(!b.anyChange);
        CHECK(sceneops::endBrushStroke(sc, h, b) == ops::Status::OK);
        CHECK(h.undoDepth() == 0); // nothing changed -> no history entry
        CHECK(h.usedBytes() == 0);
    }

    // 2) Vertices inside the radius -> displaced along the SURFACE (hit-face)
    //    normal: raising on the top face moves vertices strictly +Y.
    {
        HalfEdgeMesh m;
        PrimitiveParams p;
        p.cx = 0; p.cy = 0; p.cz = 0; p.size = 1.0f;
        REQUIRE(buildPrimitive(PrimType::Cube, p, m));

        brush::BrushSettings s;
        s.radius = 2.0f;
        s.strength = 0.5f;

        const Vec3 hit{0.0f, 1.0f, 0.0f};
        std::vector<ElemId> affected;
        brush::collectAffected(m, hit, s.radius, affected);
        CHECK(affected.size() == 4); // the 4 top corners (distance sqrt(2))

        std::vector<Vec3> before;
        for (const ElemId v : affected) before.push_back(m.vrt(v).pos);
        REQUIRE(brush::sculptApply(m, affected, hit, Vec3{0, 1, 0}, s));
        for (size_t i = 0; i < affected.size(); ++i) {
            const Vec3& a = before[i];
            const Vec3& b = m.vrt(affected[i]).pos;
            CHECK(b.y > a.y);            // raised along the surface normal
            CHECK_NEAR(b.x, a.x, 1e-6f); // no lateral slide
            CHECK_NEAR(b.z, a.z, 1e-6f);
        }

        // Dig mode displaces along -normal.
        s.mode = brush::SculptMode::Dig;
        std::vector<Vec3> beforeDig;
        for (const ElemId v : affected) beforeDig.push_back(m.vrt(v).pos);
        REQUIRE(brush::sculptApply(m, affected, hit, Vec3{0, 1, 0}, s));
        for (size_t i = 0; i < affected.size(); ++i) {
            CHECK(m.vrt(affected[i]).pos.y < beforeDig[i].y);
        }
    }
}

// -------------------------------------------------------------------- 3 --

void testSculptRadiusAndStrengthRespected() {
    HalfEdgeMesh m;
    PrimitiveParams p;
    p.cx = 0; p.cy = 0; p.cz = 0; p.size = 1.0f;
    REQUIRE(buildPrimitive(PrimType::Cube, p, m));

    // Radius gates membership: the corner (1,1,-1) is distance 2 from the
    // three adjacent corners; radius 1.5 selects ONLY the hit corner,
    // radius 2.5 selects the hit corner + the 3 adjacent ones.
    std::vector<ElemId> small, big;
    brush::collectAffected(m, Vec3{1.0f, 1.0f, -1.0f}, 1.5f, small);
    brush::collectAffected(m, Vec3{1.0f, 1.0f, -1.0f}, 2.5f, big);
    CHECK(small.size() == 1);
    CHECK(big.size() == 4);

    // Strength scales the displacement (monotonic; fixed step constant).
    brush::BrushSettings s;
    s.radius = 2.5f;
    const Vec3 hit{1.0f, 1.0f, -1.0f};
    const Vec3 normal{0, 0, -1}; // -Z face under the brush
    float dispStrong = 0.0f, dispWeak = 0.0f;
    {
        HalfEdgeMesh tmp;
        tmp.restoreRaw(m.captureRaw());
        s.strength = 1.0f;
        std::vector<ElemId> aff;
        brush::collectAffected(tmp, hit, s.radius, aff);
        const Vec3 v0 = tmp.vrt(aff[0]).pos;
        REQUIRE(brush::sculptApply(tmp, aff, hit, normal, s));
        dispStrong = length(tmp.vrt(aff[0]).pos - v0);
    }
    {
        HalfEdgeMesh tmp;
        tmp.restoreRaw(m.captureRaw());
        s.strength = 0.1f;
        std::vector<ElemId> aff;
        brush::collectAffected(tmp, hit, s.radius, aff);
        const Vec3 v0 = tmp.vrt(aff[0]).pos;
        REQUIRE(brush::sculptApply(tmp, aff, hit, normal, s));
        dispWeak = length(tmp.vrt(aff[0]).pos - v0);
    }
    CHECK(dispStrong > dispWeak * 5.0f); // ~10x step, same direction
    CHECK(dispWeak > 0.0f);              // weak still moves (visible feedback)

    // A vertex OUTSIDE the collected set keeps its exact position.
    HalfEdgeMesh tmp;
    tmp.restoreRaw(m.captureRaw());
    s.strength = 1.0f;
    s.radius = 1.5f;
    std::vector<ElemId> aff;
    brush::collectAffected(tmp, hit, s.radius, aff);
    const ElemId far = [&] {
        for (const auto& v : tmp.vertices()) {
            if (!v.alive) continue;
            const float d = length(v.pos - hit);
            if (d > s.radius) return v.id;
        }
        return kNull;
    }();
    REQUIRE(far != kNull);
    const Vec3 farBefore = tmp.vrt(far).pos;
    REQUIRE(brush::sculptApply(tmp, aff, hit, normal, s));
    CHECK(std::memcmp(&tmp.vrt(far).pos, &farBefore, sizeof(Vec3)) == 0);
}

// -------------------------------------------------------------------- 4 --

void testBrushRejectsInvalidValues() {
    brush::BrushSettings ok;
    ok.radius = 0.5f;
    ok.strength = 0.5f;
    ok.opacity = 0.8f;
    CHECK(brush::settingsValid(ok));

    auto invalid = [&](auto mutate) {
        brush::BrushSettings s = ok;
        mutate(s);
        return brush::settingsValid(s);
    };
    CHECK(!invalid([](brush::BrushSettings& s) { s.radius = std::nanf(""); }));
    CHECK(!invalid([](brush::BrushSettings& s) { s.radius = 0.0f; }));
    CHECK(!invalid([](brush::BrushSettings& s) { s.radius = 99.0f; }));
    CHECK(!invalid([](brush::BrushSettings& s) { s.strength = std::nanf(""); }));
    CHECK(!invalid([](brush::BrushSettings& s) { s.strength = 0.0f; }));
    CHECK(!invalid([](brush::BrushSettings& s) { s.opacity = -0.1f; }));
    CHECK(!invalid([](brush::BrushSettings& s) { s.color = Vec3{2.0f, 0.0f, 0.0f}; }));
    CHECK(!invalid([](brush::BrushSettings& s) {
        s.color = Vec3{0.0f, std::nanf(""), 0.0f};
    }));

    // Sanitizer: garbage in, safe defaults out (never propagates NaN).
    brush::BrushSettings bad = ok;
    bad.radius = std::nanf("");
    bad.strength = -3.0f;
    bad.color = Vec3{std::nanf(""), 0.5f, 0.5f};
    const brush::BrushSettings fixed = brush::sanitized(bad);
    CHECK(brush::settingsValid(fixed));

    // sculptApply/paintApply reject non-finite hits and degenerate normals.
    HalfEdgeMesh m;
    PrimitiveParams p;
    p.cx = 0; p.cy = 0; p.cz = 0; p.size = 1.0f;
    REQUIRE(buildPrimitive(PrimType::Cube, p, m));
    const HalfEdgeMesh::Raw before = m.captureRaw();
    std::vector<ElemId> aff;
    brush::collectAffected(m, Vec3{0, 0, 0}, 2.5f, aff);
    CHECK(!aff.empty());
    CHECK(!brush::sculptApply(m, aff, Vec3{std::nanf(""), 0, 0}, Vec3{0, 1, 0}, ok));
    CHECK(!brush::sculptApply(m, aff, Vec3{0, 0, 0}, Vec3{std::nanf(""), 1, 0}, ok));
    CHECK(!brush::sculptApply(m, aff, Vec3{0, 0, 0}, Vec3{0, 0, 0}, ok)); // degenerate
    CHECK(!brush::paintApply(m, aff, Vec3{0, std::nanf(""), 0}, ok));
    CHECK(test::sameMesh(m, test::meshFromRaw(before))); // untouched

    // Session level: invalid settings/hits are rejected BEFORE any mutation.
    History h;
    Scene sc = makeCubeScene(h);
    sceneops::BrushSession b;
    brush::BrushSettings bad2;
    bad2.radius = -1.0f;
    CHECK(sceneops::beginBrushStroke(sc, h, b, sceneops::BrushTool::Sculpt, 0,
                                     Vec3{1, 2, 1}, Vec3{0, 1, 0}, bad2) ==
          ops::Status::ErrInvalidArg);
    CHECK(!b.active);
    brush::BrushSettings good;
    good.radius = 1.5f;
    good.strength = 0.5f;
    CHECK(sceneops::beginBrushStroke(sc, h, b, sceneops::BrushTool::Sculpt, 0,
                                     Vec3{1, 2, 1}, Vec3{0, 1, 0}, good) ==
          ops::Status::OK);
    const HalfEdgeMesh::Raw snap = sc.objects[0].mesh.captureRaw();
    CHECK(sceneops::moveBrushStroke(sc, b, Vec3{std::nanf(""), 0, 0},
                                    Vec3{0, 1, 0}, 0.5f, 0.5f) ==
          ops::Status::ErrInvalidArg);
    CHECK(test::sameMesh(sc.objects[0].mesh, test::meshFromRaw(snap)));
    CHECK(b.active); // session survives a rejected move
    CHECK(sceneops::endBrushStroke(sc, h, b) == ops::Status::OK);

    // Unknown tool id.
    CHECK(sceneops::beginBrushStroke(sc, h, b,
                                     static_cast<sceneops::BrushTool>(9), 0,
                                     Vec3{1, 2, 1}, Vec3{0, 1, 0}, good) ==
          ops::Status::ErrInvalidArg);
    CHECK(!b.active);
}

// -------------------------------------------------------------------- 5 --

void testSculptPreservesMeshIntegrity() {
    History h;
    Scene sc = makeCubeScene(h);
    sceneops::BrushSession b;
    brush::BrushSettings s;
    s.radius = 2.0f;
    s.strength = 0.8f;

    const size_t v0 = sc.objects[0].mesh.aliveVertexCount();
    const size_t e0 = sc.objects[0].mesh.aliveEdgeCount();
    const size_t f0 = sc.objects[0].mesh.aliveFaceCount();
    const size_t h0 = sc.objects[0].mesh.aliveHalfEdgeCount();

    CHECK(sceneops::beginBrushStroke(sc, h, b, sceneops::BrushTool::Sculpt, 0,
                                     Vec3{1, 2, 1}, Vec3{0, 1, 0}, s) ==
          ops::Status::OK);
    // A drag across the top face: several overlapping applications.
    for (int i = 0; i < 12; ++i) {
        const float t = static_cast<float>(i) / 11.0f;
        const Vec3 hit{1.0f - 2.0f * t, 2.0f, 1.0f - 2.0f * t};
        CHECK(sceneops::moveBrushStroke(sc, b, hit, Vec3{0, 1, 0}, s.radius,
                                        s.strength) == ops::Status::OK);
    }
    CHECK(sceneops::endBrushStroke(sc, h, b) == ops::Status::OK);

    const HalfEdgeMesh& m = sc.objects[0].mesh;
    CHECK(m.aliveVertexCount() == v0); // topology unchanged by sculpting
    CHECK(m.aliveEdgeCount() == e0);
    CHECK(m.aliveFaceCount() == f0);
    CHECK(m.aliveHalfEdgeCount() == h0);
    CHECK(m.validate());
    test::assertInvariants(m, "apos gesto de escultura");
    test::assertNormalsOk(m);
    // All positions remain finite.
    for (const auto& v : m.vertices()) {
        if (!v.alive) continue;
        CHECK(blone::ops::isFiniteVec(v.pos.x, v.pos.y, v.pos.z));
    }
}

// ------------------------------------------------------------------ 6 + 7 --

void testPaintRegionAndUntouchedColors() {
    History h;
    Scene sc = makeCubeScene(h);
    HalfEdgeMesh& m = sc.objects[0].mesh;
    const Vec3 def = kDefaultVertexColor;

    sceneops::BrushSession b;
    brush::BrushSettings s;
    s.radius = 1.5f; // touches ONLY the hit corner (next corners are 2 away)
    s.strength = 0.5f;
    s.opacity = 1.0f;
    s.color = Vec3{1.0f, 0.0f, 0.0f}; // red

    const ElemId hitCorner = cubeCorner(m, 1, 1, 1); // (1, 2, 1)
    REQUIRE(hitCorner != kNull);
    const Vec3 hitPos = m.vrt(hitCorner).pos;

    CHECK(sceneops::beginBrushStroke(sc, h, b, sceneops::BrushTool::Paint, 0,
                                     hitPos, Vec3{0, 1, 0}, s) == ops::Status::OK);
    CHECK(sceneops::endBrushStroke(sc, h, b) == ops::Status::OK);

    // 6) ONLY the intended region changed (falloff 1 at the hit vertex).
    CHECK_NEAR(m.vrt(hitCorner).color.x, 1.0f, 1e-6f);
    CHECK_NEAR(m.vrt(hitCorner).color.y, 0.0f, 1e-6f);
    CHECK_NEAR(m.vrt(hitCorner).color.z, 0.0f, 1e-6f);
    // 7) Untouched vertices keep the EXACT default color.
    for (const auto& v : m.vertices()) {
        if (!v.alive || v.id == hitCorner) continue;
        CHECK(std::memcmp(&m.vrt(v.id).color, &def, sizeof(Vec3)) == 0);
    }

    // A second, far away stroke must not disturb the first region's color.
    sceneops::BrushSession b2;
    brush::BrushSettings s2 = s;
    s2.color = Vec3{0.0f, 1.0f, 0.0f}; // green
    const ElemId opposite = cubeCorner(m, -1, -1, -1);
    REQUIRE(opposite != kNull);
    const Vec3 oppPos = m.vrt(opposite).pos;
    CHECK(sceneops::beginBrushStroke(sc, h, b2, sceneops::BrushTool::Paint, 0,
                                     oppPos, Vec3{0, -1, 0}, s2) ==
          ops::Status::OK);
    CHECK(sceneops::endBrushStroke(sc, h, b2) == ops::Status::OK);
    CHECK_NEAR(m.vrt(hitCorner).color.x, 1.0f, 1e-6f); // first region intact
    CHECK_NEAR(m.vrt(opposite).color.y, 1.0f, 1e-6f);
    CHECK_NEAR(m.vrt(opposite).color.x, 0.0f, 1e-6f);

    // Opacity < 1 blends PARTIALLY (never jumps fully to the brush color).
    sceneops::BrushSession b3;
    brush::BrushSettings s3 = s;
    s3.opacity = 0.5f;
    s3.color = Vec3{0.0f, 0.0f, 1.0f}; // blue over red
    CHECK(sceneops::beginBrushStroke(sc, h, b3, sceneops::BrushTool::Paint, 0,
                                     hitPos, Vec3{0, 1, 0}, s3) == ops::Status::OK);
    CHECK(sceneops::endBrushStroke(sc, h, b3) == ops::Status::OK);
    const Vec3 blended = m.vrt(hitCorner).color;
    CHECK(blended.x > 0.4f && blended.x < 1.0f); // still partly red
    CHECK(blended.z > 0.0f && blended.z < 0.6f); // partly blue
}

// -------------------------------------------------------------------- 8 --

void testSculptUndoRedo() {
    History h;
    Scene sc = makeCubeScene(h);
    h.clear();
    HalfEdgeMesh& m = sc.objects[0].mesh;

    const HalfEdgeMesh::Raw start = m.captureRaw();

    sceneops::BrushSession b;
    brush::BrushSettings s;
    s.radius = 2.0f;
    s.strength = 0.7f;

    // ONE gesture: begin + several moves + end => EXACTLY ONE history entry.
    CHECK(sceneops::beginBrushStroke(sc, h, b, sceneops::BrushTool::Sculpt, 0,
                                     Vec3{1, 2, 1}, Vec3{0, 1, 0}, s) ==
          ops::Status::OK);
    for (int i = 0; i < 6; ++i) {
        CHECK(sceneops::moveBrushStroke(sc, b, Vec3{0.5f * i, 2, 0.5f * i},
                                        Vec3{0, 1, 0}, s.radius,
                                        s.strength) == ops::Status::OK);
    }
    CHECK(sceneops::endBrushStroke(sc, h, b) == ops::Status::OK);

    CHECK(h.undoDepth() == 1); // one coherent entry, NOT one per frame
    CHECK(h.redoDepth() == 0);
    const HalfEdgeMesh::Raw after = m.captureRaw();
    CHECK(!test::sameMesh(m, test::meshFromRaw(start))); // something moved

    // Undo restores the exact pre-gesture positions (bit-exact).
    CHECK(sceneops::undo(sc, h) == ops::Status::OK);
    CHECK(samePositions(m, test::meshFromRaw(start)));
    CHECK(h.redoDepth() == 1);

    // Redo restores the exact post-gesture positions.
    CHECK(sceneops::redo(sc, h) == ops::Status::OK);
    CHECK(samePositions(m, test::meshFromRaw(after)));

    // Colors were never touched by the sculpt entry.
    for (const auto& v : m.vertices()) {
        if (!v.alive) continue;
        CHECK(std::memcmp(&m.vrt(v.id).color, &kDefaultVertexColor,
                          sizeof(Vec3)) == 0);
    }
}

// -------------------------------------------------------------------- 9 --

void testPaintUndoRedo() {
    History h;
    Scene sc = makeCubeScene(h);
    h.clear();
    HalfEdgeMesh& m = sc.objects[0].mesh;

    sceneops::BrushSession b;
    brush::BrushSettings s;
    s.radius = 1.5f;
    s.strength = 0.5f;
    s.opacity = 1.0f;
    s.color = Vec3{1.0f, 0.5f, 0.0f};

    CHECK(sceneops::beginBrushStroke(sc, h, b, sceneops::BrushTool::Paint, 0,
                                     Vec3{1, 2, 1}, Vec3{0, 1, 0}, s) ==
          ops::Status::OK);
    CHECK(sceneops::moveBrushStroke(sc, b, Vec3{1, 2, 0.2f}, Vec3{0, 1, 0},
                                    s.radius, s.strength) == ops::Status::OK);
    CHECK(sceneops::endBrushStroke(sc, h, b) == ops::Status::OK);

    CHECK(h.undoDepth() == 1);
    const HalfEdgeMesh::Raw painted = m.captureRaw();

    // Positions were NEVER touched by the paint stroke (capture == live).
    CHECK(samePositions(m, test::meshFromRaw(painted)));

    CHECK(sceneops::undo(sc, h) == ops::Status::OK);
    for (const auto& v : m.vertices()) {
        if (!v.alive) continue;
        CHECK(std::memcmp(&m.vrt(v.id).color, &kDefaultVertexColor,
                          sizeof(Vec3)) == 0); // colors restored exactly
    }
    CHECK(sceneops::redo(sc, h) == ops::Status::OK);
    CHECK(sameColors(m, test::meshFromRaw(painted)));
}

// ------------------------------------------------------------------- 10 --

void testCancelBrushStroke() {
    // Sculpt cancel restores the exact start positions.
    {
        History h;
        Scene sc = makeCubeScene(h);
        h.clear();
        HalfEdgeMesh& m = sc.objects[0].mesh;
        const HalfEdgeMesh::Raw start = m.captureRaw();

        sceneops::BrushSession b;
        brush::BrushSettings s;
        s.radius = 2.0f;
        s.strength = 1.0f;
        CHECK(sceneops::beginBrushStroke(sc, h, b, sceneops::BrushTool::Sculpt, 0,
                                         Vec3{1, 2, 1}, Vec3{0, 1, 0}, s) ==
              ops::Status::OK);
        for (int i = 0; i < 4; ++i) {
            CHECK(sceneops::moveBrushStroke(sc, b, Vec3{0.3f * i, 2, 0},
                                            Vec3{0, 1, 0}, s.radius,
                                            s.strength) == ops::Status::OK);
        }
        sceneops::cancelBrushStroke(sc, h, b);
        CHECK(!b.active);
        CHECK(samePositions(m, test::meshFromRaw(start))); // restored exactly
        CHECK(h.undoDepth() == 0); // a cancelled gesture leaves NO entry
        CHECK(h.usedBytes() == 0);
    }
    // Paint cancel restores the exact start colors.
    {
        History h;
        Scene sc = makeCubeScene(h);
        h.clear();
        HalfEdgeMesh& m = sc.objects[0].mesh;
        sceneops::BrushSession b;
        brush::BrushSettings s;
        s.radius = 2.5f;
        s.opacity = 1.0f;
        s.color = Vec3{0.2f, 0.9f, 0.4f};
        CHECK(sceneops::beginBrushStroke(sc, h, b, sceneops::BrushTool::Paint, 0,
                                         Vec3{0, 1, 0}, Vec3{0, 1, 0}, s) ==
              ops::Status::OK);
        sceneops::cancelBrushStroke(sc, h, b);
        CHECK(!b.active);
        for (const auto& v : m.vertices()) {
            if (!v.alive) continue;
            CHECK(std::memcmp(&m.vrt(v.id).color, &kDefaultVertexColor,
                              sizeof(Vec3)) == 0);
        }
        CHECK(h.undoDepth() == 0);
    }
}

// ------------------------------------------------------------------- 11 --

void testPersistenceV2RoundTrip() {
    History h;
    Scene sc = makeCubeScene(h);
    h.clear();

    // Sculpt a bump + paint the top-front-right corner red.
    sceneops::BrushSession b;
    brush::BrushSettings s;
    s.radius = 2.0f;
    s.strength = 0.6f;
    CHECK(sceneops::beginBrushStroke(sc, h, b, sceneops::BrushTool::Sculpt, 0,
                                     Vec3{1, 2, 1}, Vec3{0, 1, 0}, s) ==
          ops::Status::OK);
    CHECK(sceneops::endBrushStroke(sc, h, b) == ops::Status::OK);
    brush::BrushSettings ps;
    ps.radius = 1.5f;
    ps.opacity = 1.0f;
    ps.color = Vec3{1.0f, 0.0f, 0.0f};
    sceneops::BrushSession pb;
    const ElemId corner = cubeCorner(sc.objects[0].mesh, 1, 1, 1);
    REQUIRE(corner != kNull);
    const Vec3 cornerPos = sc.objects[0].mesh.vrt(corner).pos;
    CHECK(sceneops::beginBrushStroke(sc, h, pb, sceneops::BrushTool::Paint, 0,
                                     cornerPos, Vec3{0, 1, 0}, ps) ==
          ops::Status::OK);
    CHECK(sceneops::endBrushStroke(sc, h, pb) == ops::Status::OK);

    const HalfEdgeMesh& m = sc.objects[0].mesh;
    const Vec3 paintedColor = m.vrt(corner).color;
    const Vec3 paintedPos = m.vrt(corner).pos;

    const std::vector<uint8_t> bytes = project::save(sc);
    CHECK(!bytes.empty());
    CHECK(bytes.size() > 24);
    // The saved header must advertise the CURRENT format version
    // (Fase 5.1: v4 — sparse per-corner uv section; mandated bump, same
    // pattern as the v2->v3 bump in F4-LIMITS P5).
    uint32_t savedVersion = 0;
    std::memcpy(&savedVersion, bytes.data() + 8, 4);
    CHECK(savedVersion == project::kFormatVersion);
    CHECK(savedVersion == 4u);

    Scene loaded;
    REQUIRE(project::load(bytes, loaded) == project::Ok);
    REQUIRE(loaded.objects.size() == 1);
    const HalfEdgeMesh& lm = loaded.objects[0].mesh;
    // Sculpted geometry survived.
    CHECK(std::memcmp(&lm.vrt(corner).pos, &paintedPos, sizeof(Vec3)) == 0);
    // Painted color survived EXACTLY.
    CHECK(std::memcmp(&lm.vrt(corner).color, &paintedColor, sizeof(Vec3)) == 0);
    // Unpainted vertices keep the default color.
    for (const auto& v : lm.vertices()) {
        if (!v.alive || v.id == corner) continue;
        CHECK(std::memcmp(&lm.vrt(v.id).color, &kDefaultVertexColor,
                          sizeof(Vec3)) == 0);
    }
    CHECK(lm.validate());
}

// ------------------------------------------------------------------- 12 --

// Minimal CRC32 (same polynomial as Project.cpp) for crafting v1 bytes.
uint32_t testCrc32(const uint8_t* data, size_t len) {
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; ++i) {
        c ^= data[i];
        for (int k = 0; k < 8; ++k) {
            c = (c & 1u) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
        }
    }
    return c ^ 0xFFFFFFFFu;
}

void appendU32(std::vector<uint8_t>& b, uint32_t v) {
    b.push_back(static_cast<uint8_t>(v));
    b.push_back(static_cast<uint8_t>(v >> 8));
    b.push_back(static_cast<uint8_t>(v >> 16));
    b.push_back(static_cast<uint8_t>(v >> 24));
}
void appendF32(std::vector<uint8_t>& b, float f) {
    uint32_t v;
    std::memcpy(&v, &f, 4);
    appendU32(b, v);
}

// Hand-crafts a v1 BLONEPRJ file with ONE quad object (no vertex colors —
// the v1 layout). Pins the backward-compatibility contract.
std::vector<uint8_t> craftV1QuadFile() {
    // Quad: 4 verts, 4 edges, 8 half-edges, 1 face (face loop + boundary loop).
    struct V { uint32_t id; float x, y, z; };
    const V verts[4] = {{1, -1, 0, -1}, {2, 1, 0, -1}, {3, 1, 0, 1}, {4, -1, 0, 1}};
    struct H { uint32_t id, origin, twin, next, prev, face, edge; };
    const H hes[8] = {
            {1, 1, 5, 2, 4, 1, 1}, {2, 2, 6, 3, 1, 1, 2},
            {3, 3, 7, 4, 2, 1, 3}, {4, 4, 8, 1, 3, 1, 4},
            {5, 2, 1, 8, 6, 0, 1}, {6, 3, 2, 5, 7, 0, 2},
            {7, 4, 3, 6, 8, 0, 3}, {8, 1, 4, 7, 5, 0, 4},
    };
    const uint32_t edges[4][2] = {{1, 1}, {2, 2}, {3, 3}, {4, 4}};
    const uint32_t faces[1][2] = {{1, 1}};

    std::vector<uint8_t> payload;
    appendU32(payload, 0);                    // activeObject
    payload.push_back(0);                     // editMode
    payload.push_back(2);                     // selectionMode = face
    payload.push_back(0);                     // multiSelect
    payload.push_back(0);                     // reserved
    appendU32(payload, 1);                    // objectCount

    const char* name = "Plano";
    appendU32(payload, 5);
    payload.insert(payload.end(), name, name + 5);
    appendF32(payload, 0.0f); appendF32(payload, 0.0f); appendF32(payload, 0.0f);

    appendU32(payload, 4);                    // vertexCount (v1: NO colors)
    for (const V& v : verts) {
        appendU32(payload, v.id);
        appendF32(payload, v.x); appendF32(payload, v.y); appendF32(payload, v.z);
    }
    appendU32(payload, 8);                    // heCount
    for (const H& h : hes) {
        appendU32(payload, h.id); appendU32(payload, h.origin);
        appendU32(payload, h.twin); appendU32(payload, h.next);
        appendU32(payload, h.prev); appendU32(payload, h.face);
        appendU32(payload, h.edge);
    }
    appendU32(payload, 4);                    // edgeCount
    for (const auto& e : edges) {
        appendU32(payload, e[0]); appendU32(payload, e[1]);
    }
    appendU32(payload, 1);                    // faceCount
    for (const auto& f : faces) {
        appendU32(payload, f[0]); appendU32(payload, f[1]);
    }
    appendU32(payload, 0); appendU32(payload, 0); // id counters (recomputed)
    appendU32(payload, 0); appendU32(payload, 0);
    appendU32(payload, 0);                    // selV
    appendU32(payload, 0);                    // selE
    appendU32(payload, 1); appendU32(payload, 1); // selF = face 1

    std::vector<uint8_t> file;
    file.insert(file.end(), {'B', 'L', 'O', 'N', 'E', 'P', 'R', 'J'});
    appendU32(file, 1); // formatVersion 1
    const uint64_t len = payload.size();
    for (int i = 0; i < 8; ++i) {
        file.push_back(static_cast<uint8_t>(len >> (8 * i)));
    }
    appendU32(file, testCrc32(payload.data(), payload.size()));
    file.insert(file.end(), payload.begin(), payload.end());
    return file;
}

void testV1FileCompatibility() {
    const std::vector<uint8_t> v1 = craftV1QuadFile();
    Scene loaded;
    REQUIRE(project::load(v1, loaded) == project::Ok);
    REQUIRE(loaded.objects.size() == 1);
    const HalfEdgeMesh& m = loaded.objects[0].mesh;
    CHECK(m.aliveVertexCount() == 4);
    CHECK(m.aliveFaceCount() == 1);
    CHECK(m.validate());
    // v1 files have NO colors: every vertex loads with the default color,
    // identical to how the mesh rendered before Fase 3.
    for (const auto& v : m.vertices()) {
        if (!v.alive) continue;
        CHECK(std::memcmp(&v.color, &kDefaultVertexColor, sizeof(Vec3)) == 0);
    }
    // A painted v1-loaded mesh saves back as v2 and keeps everything.
    History h;
    sceneops::BrushSession b;
    brush::BrushSettings s;
    s.radius = 1.5f;
    s.opacity = 1.0f;
    s.color = Vec3{0.1f, 0.2f, 0.3f};
    CHECK(sceneops::beginBrushStroke(loaded, h, b, sceneops::BrushTool::Paint, 0,
                                     m.vrt(1).pos, Vec3{0, 1, 0}, s) ==
          ops::Status::OK);
    CHECK(sceneops::endBrushStroke(loaded, h, b) == ops::Status::OK);
    const std::vector<uint8_t> v3 = project::save(loaded);
    CHECK(!v3.empty());
    uint32_t version = 0;
    std::memcpy(&version, v3.data() + 8, 4);
    // Fase 5.1: the writer emits the CURRENT version (4 — sparse per-corner
    // uv section); same mandated bump as the v2->v3 one in F4-LIMITS P5.
    CHECK(version == 4u);
}

// -------------------------------------------------------- budget + misc --

void testBrushBudgetAndGuards() {
    History h;
    Scene sc = makeCubeScene(h);
    h.clear();
    h.setBudget(256); // a cube stroke bound (8*32+256 = 512) will not fit
    sceneops::BrushSession b;
    brush::BrushSettings s;
    s.radius = 1.5f;
    s.strength = 0.5f;
    const HalfEdgeMesh::Raw before = sc.objects[0].mesh.captureRaw();
    CHECK(sceneops::beginBrushStroke(sc, h, b, sceneops::BrushTool::Sculpt, 0,
                                     Vec3{1, 2, 1}, Vec3{0, 1, 0}, s) ==
          ops::Status::ErrBudget);
    CHECK(!b.active);
    CHECK(test::sameMesh(sc.objects[0].mesh, test::meshFromRaw(before)));
    CHECK(h.undoDepth() == 0);

    // Double-begin is defensive-cancelled (one stroke at a time).
    h.setBudget(History::kDefaultBudget);
    CHECK(sceneops::beginBrushStroke(sc, h, b, sceneops::BrushTool::Sculpt, 0,
                                     Vec3{1, 2, 1}, Vec3{0, 1, 0}, s) ==
          ops::Status::OK);
    CHECK(b.active);
    // Starting a second stroke while the first is active cancels the first
    // (its dab is rolled back), then starts cleanly.
    CHECK(sceneops::beginBrushStroke(sc, h, b, sceneops::BrushTool::Paint, 0,
                                     Vec3{1, 0, 1}, Vec3{0, -1, 0}, s) ==
          ops::Status::OK);
    CHECK(b.active);
    CHECK(b.tool == sceneops::BrushTool::Paint);
    sceneops::cancelBrushStroke(sc, h, b);
    CHECK(!b.active);

    // End without an active session is rejected, not fatal.
    CHECK(sceneops::endBrushStroke(sc, h, b) == ops::Status::ErrInvalidArg);
    // Invalid object index.
    CHECK(sceneops::beginBrushStroke(sc, h, b, sceneops::BrushTool::Sculpt, 7,
                                     Vec3{1, 2, 1}, Vec3{0, 1, 0}, s) ==
          ops::Status::ErrNoActiveObject);
    CHECK(!b.active);
}

} // namespace

namespace blone::test {

void runBrushTests() {
    std::printf("-- brush (esculpir/pintar) --\n");
    testSculptCollectAndApply();
    testSculptRadiusAndStrengthRespected();
    testBrushRejectsInvalidValues();
    testSculptPreservesMeshIntegrity();
    testPaintRegionAndUntouchedColors();
    testSculptUndoRedo();
    testPaintUndoRedo();
    testCancelBrushStroke();
    testPersistenceV2RoundTrip();
    testV1FileCompatibility();
    testBrushBudgetAndGuards();
}

} // namespace blone::test
