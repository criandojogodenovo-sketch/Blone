#include "Project.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace blone::project {

namespace {

// ---------------------------------------------------------------- CRC32 ---
struct Crc32Table {
    uint32_t v[256];
    Crc32Table() {
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k) {
                c = (c & 1u) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            }
            v[i] = c;
        }
    }
};
const Crc32Table kCrc = [] {
    return Crc32Table{};
}();

uint32_t crc32(const uint8_t* data, size_t len) {
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; ++i) {
        c = kCrc.v[(c ^ data[i]) & 0xFFu] ^ (c >> 8);
    }
    return c ^ 0xFFFFFFFFu;
}

// ------------------------------------------------------------- writer ---
struct Writer {
    std::vector<uint8_t> buf;
    void u8(uint8_t v) { buf.push_back(v); }
    void u32(uint32_t v) {
        buf.push_back(static_cast<uint8_t>(v));
        buf.push_back(static_cast<uint8_t>(v >> 8));
        buf.push_back(static_cast<uint8_t>(v >> 16));
        buf.push_back(static_cast<uint8_t>(v >> 24));
    }
    void i32(int32_t v) { u32(static_cast<uint32_t>(v)); }
    void u64(uint64_t v) {
        for (int i = 0; i < 8; ++i) buf.push_back(static_cast<uint8_t>(v >> (8 * i)));
    }
    void f32(float f) {
        uint32_t v;
        static_assert(sizeof(v) == sizeof(f), "float size");
        std::memcpy(&v, &f, sizeof(v));
        u32(v);
    }
    void bytes(const uint8_t* p, size_t n) { buf.insert(buf.end(), p, p + n); }
};

void writeMesh(Writer& w, const HalfEdgeMesh& m) {
    // Alive elements only, ordered by id for determinism.
    std::vector<const HalfEdgeMesh::Vertex*> vs;
    std::vector<const HalfEdgeMesh::HalfEdge*> hs;
    std::vector<const HalfEdgeMesh::Edge*> es;
    std::vector<const HalfEdgeMesh::Face*> fs;
    for (const auto& el : m.vertices()) if (el.alive) vs.push_back(&el);
    for (const auto& el : m.halfEdges()) if (el.alive) hs.push_back(&el);
    for (const auto& el : m.edges()) if (el.alive) es.push_back(&el);
    for (const auto& el : m.faces()) if (el.alive) fs.push_back(&el);

    w.u32(static_cast<uint32_t>(vs.size()));
    for (const auto* v : vs) {
        w.u32(v->id);
        w.f32(v->pos.x);
        w.f32(v->pos.y);
        w.f32(v->pos.z);
        // v2: albedo color per vertex (Fase 3).
        w.f32(v->color.x);
        w.f32(v->color.y);
        w.f32(v->color.z);
    }
    w.u32(static_cast<uint32_t>(hs.size()));
    for (const auto* h : hs) {
        w.u32(h->id);
        w.u32(h->origin);
        w.u32(h->twin);
        w.u32(h->next);
        w.u32(h->prev);
        w.u32(h->face);
        w.u32(h->edge);
    }
    w.u32(static_cast<uint32_t>(es.size()));
    for (const auto* e : es) {
        w.u32(e->id);
        w.u32(e->he);
    }
    w.u32(static_cast<uint32_t>(fs.size()));
    for (const auto* f : fs) {
        w.u32(f->id);
        w.u32(f->he);
    }
    // Id counters are not exposed; they only need to exceed every saved id.
    // The loader recomputes them as maxId + 1, so zeros are written here.
    w.u32(0);
    w.u32(0);
    w.u32(0);
    w.u32(0);
}

// v3 (F4-LIMITS): the READ-ONLY render-mesh payload (see the header layout).
void writeRenderMesh(Writer& w, const render::RenderMesh& rm) {
    w.u32(static_cast<uint32_t>(rm.positions.size()));
    const bool hasColors = rm.colors.size() == rm.positions.size();
    for (size_t i = 0; i < rm.positions.size(); ++i) {
        const Vec3& p = rm.positions[i];
        w.f32(p.x);
        w.f32(p.y);
        w.f32(p.z);
        const Vec3 c = hasColors ? rm.colors[i] : Vec3{1.0f, 1.0f, 1.0f};
        w.f32(c.x);
        w.f32(c.y);
        w.f32(c.z);
    }
    w.u32(static_cast<uint32_t>(rm.indices.size()));
    for (const uint32_t idx : rm.indices) w.u32(idx);
}

void writeSelection(Writer& w, const Selection& sel) {
    w.u32(static_cast<uint32_t>(sel.verts.size()));
    for (const ElemId v : sel.verts) w.u32(v);
    w.u32(static_cast<uint32_t>(sel.edges.size()));
    for (const ElemId e : sel.edges) w.u32(e);
    w.u32(static_cast<uint32_t>(sel.faces.size()));
    for (const ElemId f : sel.faces) w.u32(f);
}

// ------------------------------------------------------------- reader ---
struct Reader {
    const uint8_t* p;
    size_t len;
    size_t off = 0;

    bool take(size_t n) {
        if (len - off < n) return false;
        off += n;
        return true;
    }
    bool u8(uint8_t& v) {
        if (!take(1)) return false;
        v = p[off - 1];
        return true;
    }
    bool u32(uint32_t& v) {
        if (!take(4)) return false;
        v = static_cast<uint32_t>(p[off - 4]) | (static_cast<uint32_t>(p[off - 3]) << 8) |
            (static_cast<uint32_t>(p[off - 2]) << 16) | (static_cast<uint32_t>(p[off - 1]) << 24);
        return true;
    }
    bool i32(int32_t& v) {
        uint32_t t;
        if (!u32(t)) return false;
        std::memcpy(&v, &t, sizeof(v));
        return true;
    }
    bool u64(uint64_t& v) {
        v = 0;
        for (int i = 0; i < 8; ++i) {
            uint8_t b;
            if (!u8(b)) return false;
            v |= static_cast<uint64_t>(b) << (8 * i);
        }
        return true;
    }
    bool f32(float& f) {
        uint32_t v;
        if (!u32(v)) return false;
        std::memcpy(&f, &v, sizeof(f));
        return true;
    }
};

int readMesh(Reader& r, HalfEdgeMesh& m, uint32_t fmtVersion) {
    HalfEdgeMesh::Raw raw;
    uint32_t n = 0;
    if (!r.u32(n)) return ErrTruncated;
    if (n > MeshLimits::maxVertices) return ErrLimits;
    raw.verts.resize(n);
    for (uint32_t i = 0; i < n; ++i) {
        auto& v = raw.verts[i];
        if (!r.u32(v.id) || !r.f32(v.pos.x) || !r.f32(v.pos.y) || !r.f32(v.pos.z)) {
            return ErrTruncated;
        }
        if (v.id == kNull) return ErrIndex;
        if (fmtVersion >= 2) {
            if (!r.f32(v.color.x) || !r.f32(v.color.y) || !r.f32(v.color.z)) {
                return ErrTruncated;
            }
            // Defensive color validation: reject non-finite (corrupt), clamp
            // out-of-range finite values into [0,1] (never guess beyond that).
            if (!std::isfinite(v.color.x) || !std::isfinite(v.color.y) || !std::isfinite(v.color.z)) return ErrIndex;
            v.color.x = std::min(1.0f, std::max(0.0f, v.color.x));
            v.color.y = std::min(1.0f, std::max(0.0f, v.color.y));
            v.color.z = std::min(1.0f, std::max(0.0f, v.color.z));
        } else {
            v.color = kDefaultVertexColor; // v1 files have no colors
        }
        v.alive = true;
    }
    if (!r.u32(n)) return ErrTruncated;
    if (n > MeshLimits::maxHalfEdges) return ErrLimits;
    raw.hes.resize(n);
    for (uint32_t i = 0; i < n; ++i) {
        auto& h = raw.hes[i];
        if (!r.u32(h.id) || !r.u32(h.origin) || !r.u32(h.twin) || !r.u32(h.next) ||
            !r.u32(h.prev) || !r.u32(h.face) || !r.u32(h.edge)) {
            return ErrTruncated;
        }
        if (h.id == kNull || h.origin == kNull || h.twin == kNull || h.edge == kNull) {
            return ErrIndex;
        }
        h.alive = true;
    }
    if (!r.u32(n)) return ErrTruncated;
    if (n > MeshLimits::maxEdges) return ErrLimits;
    raw.edges.resize(n);
    for (uint32_t i = 0; i < n; ++i) {
        auto& e = raw.edges[i];
        if (!r.u32(e.id) || !r.u32(e.he)) return ErrTruncated;
        if (e.id == kNull || e.he == kNull) return ErrIndex;
        e.alive = true;
    }
    if (!r.u32(n)) return ErrTruncated;
    if (n > MeshLimits::maxFaces) return ErrLimits;
    raw.faces.resize(n);
    for (uint32_t i = 0; i < n; ++i) {
        auto& f = raw.faces[i];
        if (!r.u32(f.id) || !r.u32(f.he)) return ErrTruncated;
        if (f.id == kNull || f.he == kNull) return ErrIndex;
        f.alive = true;
    }
    uint32_t counters[4];
    for (auto& c : counters) {
        if (!r.u32(c)) return ErrTruncated;
    }
    // Rebuild counters as maxId + 1 (defensive against corrupted values).
    // vertex.outgoing is DERIVED state and is not serialized: recompute it as
    // the first alive half-edge of each vertex (same rule the mesh uses).
    ElemId maxV = 0, maxH = 0, maxE = 0, maxF = 0;
    for (const auto& v : raw.verts) maxV = std::max(maxV, v.id);
    for (const auto& h : raw.hes) maxH = std::max(maxH, h.id);
    for (const auto& e : raw.edges) maxE = std::max(maxE, e.id);
    for (const auto& f : raw.faces) maxF = std::max(maxF, f.id);
    if (maxV == 0xFFFFFFFFu || maxH == 0xFFFFFFFFu || maxE == 0xFFFFFFFFu ||
        maxF == 0xFFFFFFFFu) {
        return ErrLimits;
    }
    raw.nextVid = maxV + 1;
    raw.nextHid = maxH + 1;
    raw.nextEid = maxE + 1;
    raw.nextFid = maxF + 1;

    // Compact pools: slot == position; id->slot maps sized by maxId.
    raw.aliveV = raw.verts.size();
    raw.aliveH = raw.hes.size();
    raw.aliveE = raw.edges.size();
    raw.aliveF = raw.faces.size();
    raw.vSlot.resize(static_cast<size_t>(maxV) + 1);
    for (size_t i = 0; i < raw.verts.size(); ++i) {
        if (raw.verts[i].id >= raw.vSlot.size()) return ErrIndex;
        raw.vSlot[raw.verts[i].id] = static_cast<ElemId>(i);
    }
    raw.hSlot.resize(static_cast<size_t>(maxH) + 1);
    for (size_t i = 0; i < raw.hes.size(); ++i) {
        if (raw.hes[i].id >= raw.hSlot.size()) return ErrIndex;
        raw.hSlot[raw.hes[i].id] = static_cast<ElemId>(i);
    }
    raw.eSlot.resize(static_cast<size_t>(maxE) + 1);
    for (size_t i = 0; i < raw.edges.size(); ++i) {
        if (raw.edges[i].id >= raw.eSlot.size()) return ErrIndex;
        raw.eSlot[raw.edges[i].id] = static_cast<ElemId>(i);
    }
    raw.fSlot.resize(static_cast<size_t>(maxF) + 1);
    for (size_t i = 0; i < raw.faces.size(); ++i) {
        if (raw.faces[i].id >= raw.fSlot.size()) return ErrIndex;
        raw.fSlot[raw.faces[i].id] = static_cast<ElemId>(i);
    }
    for (auto& v : raw.verts) v.outgoing = kNull;
    for (auto& h : raw.hes) {
        if (h.origin >= raw.vSlot.size()) return ErrIndex;
        auto& v = raw.verts[raw.vSlot[h.origin]];
        if (v.outgoing == kNull) v.outgoing = h.id;
    }

    m.restoreRaw(raw);
    if (!m.validate()) return ErrTopology;
    return Ok;
}

int readRenderMesh(Reader& r, SceneObject& obj) {
    // v3 READ-ONLY payload -> a shared immutable RenderMesh.
    uint32_t n = 0;
    if (!r.u32(n)) return ErrTruncated;
    if (n > MeshLimits::maxVertices) return ErrLimits;
    std::vector<Vec3> positions(n);
    std::vector<Vec3> colors(n);
    for (uint32_t i = 0; i < n; ++i) {
        if (!r.f32(positions[i].x) || !r.f32(positions[i].y) ||
            !r.f32(positions[i].z) || !r.f32(colors[i].x) ||
            !r.f32(colors[i].y) || !r.f32(colors[i].z)) {
            return ErrTruncated;
        }
        if (!std::isfinite(positions[i].x) || !std::isfinite(positions[i].y) ||
            !std::isfinite(positions[i].z) || !std::isfinite(colors[i].x) ||
            !std::isfinite(colors[i].y) || !std::isfinite(colors[i].z)) {
            return ErrIndex;
        }
        colors[i].x = std::min(1.0f, std::max(0.0f, colors[i].x));
        colors[i].y = std::min(1.0f, std::max(0.0f, colors[i].y));
        colors[i].z = std::min(1.0f, std::max(0.0f, colors[i].z));
    }
    if (!r.u32(n)) return ErrTruncated;
    // Indices: 3 per triangle, every value inside the vertex pool.
    if (n % 3 != 0 || n / 3 > MeshLimits::maxFaces) return ErrLimits;
    std::vector<uint32_t> indices(n);
    for (uint32_t i = 0; i < n; ++i) {
        if (!r.u32(indices[i])) return ErrTruncated;
        if (positions.empty() || indices[i] >= positions.size()) {
            return ErrIndex;
        }
    }

    blone::render::RenderMesh rm;
    rm.positions = std::move(positions);
    rm.colors = std::move(colors);
    rm.indices = std::move(indices);
    rm.triangleCount = rm.indices.size() / 3;
    Vec3 acc{};
    size_t count = 0;
    if (!rm.positions.empty()) {
        Aabb b;
        b.minX = b.maxX = rm.positions[0].x;
        b.minY = b.maxY = rm.positions[0].y;
        b.minZ = b.maxZ = rm.positions[0].z;
        for (const Vec3& p : rm.positions) {
            b.minX = std::min(b.minX, p.x);
            b.minY = std::min(b.minY, p.y);
            b.minZ = std::min(b.minZ, p.z);
            b.maxX = std::max(b.maxX, p.x);
            b.maxY = std::max(b.maxY, p.y);
            b.maxZ = std::max(b.maxZ, p.z);
            acc = acc + p;
            ++count;
        }
        rm.bounds = b;
    }
    if (count > 0) rm.centroid = acc * (1.0f / static_cast<float>(count));

    obj.editable = false;
    obj.readMesh = std::make_shared<const blone::render::RenderMesh>(
            std::move(rm));
    return Ok;
}
int readSelection(Reader& r, HalfEdgeMesh& m, Selection& sel) {
    auto readSet = [&](Selection::Set& out, bool (HalfEdgeMesh::*alive)(ElemId) const) -> int {
        uint32_t n;
        if (!r.u32(n)) return ErrTruncated;
        if (n > MeshLimits::maxVertices) return ErrLimits;
        for (uint32_t i = 0; i < n; ++i) {
            uint32_t id;
            if (!r.u32(id)) return ErrTruncated;
            if (!(m.*alive)(id)) return ErrIndex;
            out.insert(id);
        }
        return Ok;
    };
    int rc = readSet(sel.verts, &HalfEdgeMesh::vertexAlive);
    if (rc != Ok) return rc;
    rc = readSet(sel.edges, &HalfEdgeMesh::edgeAlive);
    if (rc != Ok) return rc;
    return readSet(sel.faces, &HalfEdgeMesh::faceAlive);
}

} // namespace

std::vector<uint8_t> save(const Scene& scene) {
    Writer w;
    const char magic[8] = {'B', 'L', 'O', 'N', 'E', 'P', 'R', 'J'};
    w.bytes(reinterpret_cast<const uint8_t*>(magic), 8);
    w.u32(kFormatVersion);
    w.u64(0); // placeholder for payload length
    w.u32(0); // placeholder for CRC

    const size_t payloadStart = w.buf.size();
    w.i32(scene.active);
    w.u8(scene.editMode ? 1 : 0);
    w.u8(static_cast<uint8_t>(scene.selectionMode));
    w.u8(scene.multiSelect ? 1 : 0);
    w.u8(0);
    w.u32(static_cast<uint32_t>(scene.objects.size()));
    for (const SceneObject& obj : scene.objects) {
        const uint32_t nameLen =
                std::min<size_t>(obj.name.size(), kMaxNameBytes);
        w.u32(nameLen);
        w.bytes(reinterpret_cast<const uint8_t*>(obj.name.data()), nameLen);
        w.f32(obj.position.x);
        w.f32(obj.position.y);
        w.f32(obj.position.z);
        // v3 (F4-LIMITS): the per-object editable flag decides the payload.
        w.u8(obj.editable ? 1 : 0);
        if (!obj.editable && obj.readMesh) {
            writeRenderMesh(w, *obj.readMesh);
            // Selection is always empty on read-only objects; keep the
            // section for layout regularity.
            writeSelection(w, obj.sel);
        } else {
            writeMesh(w, obj.mesh);
            writeSelection(w, obj.sel);
        }
    }
    const size_t payloadEnd = w.buf.size();
    const size_t payloadLen = payloadEnd - payloadStart;
    const uint32_t crc = crc32(w.buf.data() + payloadStart, payloadLen);
    // Patch the header.
    const uint64_t len64 = payloadLen;
    std::memcpy(w.buf.data() + 8, &kFormatVersion, 4);
    std::memcpy(w.buf.data() + 12, &len64, 8);
    std::memcpy(w.buf.data() + 20, &crc, 4);
    return w.buf;
}

int load(const std::vector<uint8_t>& bytes, Scene& out) {
    if (bytes.size() < 24) return ErrSize;
    if (std::memcmp(bytes.data(), "BLONEPRJ", 8) != 0) return ErrMagic;
    uint32_t version;
    std::memcpy(&version, bytes.data() + 8, 4);
    // Versioned compatibility (Fase 3): v1 files (no colors) and v2 files
    // (with colors) are both accepted; anything newer is REJECTED with a
    // clear error — never silently guessed at.
    if (version < kOldestSupportedVersion || version > kFormatVersion) {
        return ErrVersion;
    }
    uint64_t payloadLen;
    std::memcpy(&payloadLen, bytes.data() + 12, 8);
    uint32_t crc;
    std::memcpy(&crc, bytes.data() + 20, 4);
    if (payloadLen != bytes.size() - 24) return ErrSize;
    if (crc32(bytes.data() + 24, static_cast<size_t>(payloadLen)) != crc) return ErrCrc;

    Reader r{bytes.data() + 24, static_cast<size_t>(payloadLen), 0};

    Scene tmp;
    if (!r.i32(tmp.active)) return ErrTruncated;
    uint8_t em = 0, sm = 0, ms = 0, pad = 0;
    if (!r.u8(em) || !r.u8(sm) || !r.u8(ms) || !r.u8(pad)) return ErrTruncated;
    tmp.editMode = em != 0;
    if (sm > 2) return ErrIndex;
    tmp.selectionMode = static_cast<SelMode>(sm);
    tmp.multiSelect = ms != 0;

    uint32_t objCount;
    if (!r.u32(objCount)) return ErrTruncated;
    if (objCount > kMaxProjectObjects) return ErrLimits;

    for (uint32_t oi = 0; oi < objCount; ++oi) {
        SceneObject obj;
        uint32_t nameLen;
        if (!r.u32(nameLen) || nameLen > kMaxNameBytes) return nameLen > kMaxNameBytes ? ErrLimits : ErrTruncated;
        obj.name.resize(nameLen);
        if (!r.take(nameLen)) return ErrTruncated;
        std::memcpy(obj.name.data(), bytes.data() + 24 + r.off - nameLen, nameLen);
        if (!r.f32(obj.position.x) || !r.f32(obj.position.y) || !r.f32(obj.position.z)) {
            return ErrTruncated;
        }
        int rc = 0;
        if (version >= 3) {
            uint8_t editable = 1;
            if (!r.u8(editable)) return ErrTruncated;
            if (editable == 0) {
                // v3 READ-ONLY object: render-mesh payload, empty selection
                // section for layout regularity.
                rc = readRenderMesh(r, obj);
                if (rc != Ok) return rc;
                Selection sel;
                rc = readSelection(r, obj.mesh, sel);
                if (rc != Ok) return rc;
                tmp.objects.push_back(std::move(obj));
                continue;
            }
        }
        rc = readMesh(r, obj.mesh, version);
        if (rc != Ok) return rc;
        rc = readSelection(r, obj.mesh, obj.sel);
        if (rc != Ok) return rc;
        tmp.objects.push_back(std::move(obj));
    }
    if (r.off != r.len) return ErrSize; // trailing garbage
    if (tmp.active >= static_cast<int>(tmp.objects.size())) return ErrIndex;
    if (tmp.objects.empty()) tmp.active = -1;

    out = std::move(tmp);
    out.bump();
    return Ok;
}

} // namespace blone::project
