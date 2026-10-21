#pragma once
// The SINGLE geometric source shared by the renderer, the picker and the
// host tests (correção Fase 2: a geometria desenhada e a malha de edição
// derivam da mesma fonte — pedido explícito do proprietário, secção 4).
//
// Contents:
//  - faceTriangles(): triangulates ONE face with EAR CLIPPING (robust for
//    concave polygons — the old fan created bowties/"Z" shapes after
//    edge-merge deletions), preserving the face winding. Quad contract
//    [a,b,c,d] -> (a,b,c),(a,c,d) is kept exactly (pinned by tests).
//  - RenderData: the full CPU-side buffers for one object. No OpenGL types,
//    so unit tests can verify buffer updates without a GL context.

#include "../mesh/HalfEdgeMesh.h"
#include "../scene/Scene.h"

#include <cstdint>
#include <vector>

namespace blone::renderdata {

// Triangulates the face loop; appends flat id triplets to `out`.
// Ear clipping on the dominant plane of the Newell normal; the winding of
// the input loop is preserved; degenerate loops fall back to a fan.
void faceTriangles(const HalfEdgeMesh& m, ElemId f, std::vector<ElemId>& out);

// CPU render buffers for one object (positions/normals interleaving is left
// to the renderer; here they are plain float arrays for testability).
struct RenderData {
    // Fill pass — flat shading: vertices DUPLICATED per face, normal per face
    // (duplicação pedida na secção 3 do relatório de correção).
    std::vector<float> fillPos;    // 3 floats per duplicated vertex
    std::vector<float> fillNrm;    // 3 floats per duplicated vertex
    std::vector<float> fillCol;    // 3 floats per duplicated vertex (albedo RGB, Fase 3)
    std::vector<uint32_t> fillIdx; // 3 per triangle (indices into fillPos)
    size_t fillVertexCount = 0;    // duplicated vertices in the render buffer
    size_t triangleCount = 0;

    // Wireframe — the real half-edges only (no triangulation diagonals).
    std::vector<float> wirePos; // 2 endpoints per edge, 3 floats each

    // Points — the true EDITABLE vertices (not the duplicated render ones).
    std::vector<float> pointPos; // 3 floats per alive vertex
    size_t editableVertexCount = 0;

    Aabb bounds{};
};

// Rebuilds everything for one object. After ANY mesh change the caller
// rebuilds (Scene::version bump -> renderer resync), so normals/positions
// always reflect the current geometry.
void buildObject(const SceneObject& obj, RenderData& out);

// Selection overlays for one object (selected faces tinted, selected edges +
// boundaries of selected faces highlighted, selected vertices as points).
struct SelectionData {
    std::vector<float> facePos;
    std::vector<float> faceNrm;
    std::vector<uint32_t> faceIdx;
    std::vector<float> edgePos; // pairs of endpoints (RGBA set by renderer)
    std::vector<float> vertPos;
};
void buildSelection(const SceneObject& obj, SelectionData& out);

} // namespace blone::renderdata
