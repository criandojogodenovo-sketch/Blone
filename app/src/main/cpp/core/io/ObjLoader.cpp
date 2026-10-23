#include "ObjLoader.h"
#include "GeometryLimits.h" // formatCount (numbered cap message, P1)

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <limits>
#include <sstream>
#include <unordered_map>

namespace blone::io {

namespace {

// ---- failure reason plumbing (hotfix D1): every rejection carries a PT
// reason + the 1-based line number so the device toast says WHY. nullptr
// callers (existing tests) pay nothing.
std::optional<RawMeshData> reject(int lineNo, const char* what,
                                  std::string* err) {
    if (err) {
        *err = "OBJ inv\u00e1lido (linha " + std::to_string(lineNo) +
               "): " + what;
    }
    return std::nullopt;
}

// ---- entry cap (missão F4-LIMITS, P1) --------------------------------------
//
// Hard cap on EVERY parsed list (positions, uvs, normals, colors, faces):
// the hard ceiling plus one, so a file beyond the engine ceiling is rejected
// DURING parsing — before any unbounded allocation — with a numbered PT
// message instead of an out-of-memory crash (P4 anti-crash, first guard).
constexpr size_t kMaxListEntries = MeshLimits::maxVertices + 1; // 4M + 1

std::optional<RawMeshData> rejectCap(int lineNo, const char* list,
                                     size_t count, std::string* err) {
    if (err) {
        *err = "OBJ inv\u00e1lido (linha " + std::to_string(lineNo) + "): " +
               list + " excede o teto do motor (" +
               formatCount(kMaxListEntries) + "; ficheiro tem pelo menos " +
               formatCount(count) + ")";
    }
    return std::nullopt;
}

// ---- strict numeric token parsing (whole token must be consumed) ----------

bool parseStrictFloat(const std::string& tok, float& out) {
    if (tok.empty()) return false;
    errno = 0;
    char* end = nullptr;
    const float v = std::strtof(tok.c_str(), &end);
    if (end != tok.c_str() + tok.size()) return false; // trailing garbage
    if (!std::isfinite(v)) return false;               // NaN / Inf rejected
    out = v;
    return true;
}

// OBJ indices are stored as written (may be negative); range checks happen
// at the resolution sites where the list sizes are known. 0 is never valid.
bool parseIndexToken(const std::string& tok, long long& out) {
    if (tok.empty()) return false;
    errno = 0;
    char* end = nullptr;
    const long long v = std::strtoll(tok.c_str(), &end, 10);
    if (end != tok.c_str() + tok.size()) return false;
    if (v == 0) return false;
    if (v > std::numeric_limits<int>::max() ||
        v < std::numeric_limits<int>::min()) {
        return false;
    }
    out = v;
    return true;
}

std::vector<std::string> splitWhitespace(const std::string& line) {
    std::vector<std::string> out;
    std::istringstream ss(line);
    std::string tok;
    while (ss >> tok) out.push_back(tok);
    return out;
}

// ---- parser state ----------------------------------------------------------
//
// Corner expansion: every unique (position, uv, normal) tuple becomes ONE
// vertex of the RawMeshData (documented in the header). Colors ride along
// from the tuple's position (white default per v line). Normals/uvs are
// filled at expansion: a corner with an explicit attribute gets it, a
// corner without gets the neutral placeholder. If NO corner in the file
// referenced vn/vt, the corresponding array is cleared at the end — an
// empty parallel array is the RawMeshData way of saying "absent".

constexpr int kAbsent = 0;      // corner field explicitly empty

// Corner-tuple map key (missão F4-LIMITS, P1): the old implementation packed
// (p,t,n) into 21-bit fields of one uint64 — the 4M hard ceiling needs 22
// bits per field (66 bits total), which NO longer fits 64 bits: packed keys
// overflowed and silently corrupted large meshes (caught by the host test
// before it could reach a device). The key is therefore a plain struct —
// correct for any ceiling ElemId can hold. Absent fields are -1.
struct CornerKey {
    int p = 0;
    int t = -1; // -1 = absent
    int n = -1; // -1 = absent
    bool operator==(const CornerKey& o) const {
        return p == o.p && t == o.t && n == o.n;
    }
};
struct CornerKeyHash {
    size_t operator()(const CornerKey& k) const {
        size_t h = static_cast<size_t>(static_cast<uint32_t>(k.p)) * 0x9E3779B9u;
        h ^= static_cast<size_t>(static_cast<uint32_t>(k.t)) * 0x85EBCA6Bu;
        h ^= static_cast<size_t>(static_cast<uint32_t>(k.n)) * 0xC2B2AE35u;
        return h;
    }
};

struct ObjParserState {
    std::vector<Vec3> pos;
    std::vector<Vec3> nrm;
    std::vector<Vec2> uv;
    std::vector<Vec3> color; // per position (white default)

    std::unordered_map<CornerKey, int, CornerKeyHash> cornerMap; // tuple -> expanded id
    bool anyNormal = false;
    bool anyUv = false;

    RawMeshData out;
};

// Resolves an OBJ index against its list (1-based positive, negative
// relative). Returns -1 on out-of-range / forward reference.
int resolve(long long raw, size_t listSize) {
    const long long abs =
            (raw > 0) ? (raw - 1) : (static_cast<long long>(listSize) + raw);
    if (abs < 0 || abs >= static_cast<long long>(listSize)) return -1;
    return static_cast<int>(abs);
}

// Expands a corner tuple into a RawMeshData vertex (creating it on first
// use). Returns the expanded index, or -1 on bad/out-of-range indices.
int expandCorner(ObjParserState& st, long long p, long long t, long long n) {
    const int pi = resolve(p, st.pos.size());
    const int ti = (t == kAbsent) ? -1 : resolve(t, st.uv.size());
    const int ni = (n == kAbsent) ? -1 : resolve(n, st.nrm.size());
    if (pi < 0) return -1;
    if (t != kAbsent && ti < 0) return -1;
    if (n != kAbsent && ni < 0) return -1;

    if (ni >= 0) st.anyNormal = true;
    if (ti >= 0) st.anyUv = true;

    const CornerKey key{pi, ti, ni};
    const auto it = st.cornerMap.find(key);
    if (it != st.cornerMap.end()) return it->second;

    const int id = static_cast<int>(st.out.positions.size());
    st.out.positions.push_back(st.pos[static_cast<size_t>(pi)]);
    st.out.colors.push_back(st.color[static_cast<size_t>(pi)]);
    st.out.normals.push_back(ni >= 0 ? st.nrm[static_cast<size_t>(ni)]
                                     : Vec3{0.0f, 0.0f, 0.0f});
    st.out.uvs.push_back(ti >= 0 ? st.uv[static_cast<size_t>(ti)]
                                 : Vec2{0.0f, 0.0f});
    st.cornerMap.emplace(key, id);
    return id;
}

} // namespace

std::optional<RawMeshData> ObjLoader::parseFile(std::istream& in,
                                                std::string* err) {
    if (!in) {
        if (err) *err = "OBJ inv\u00e1lido: stream n\u00e3o leg\u00edvel";
        return std::nullopt;
    }

    ObjParserState st;
    std::string line;
    int lineNo = 0;
    while (std::getline(in, line)) {
        ++lineNo;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const auto hash = line.find('#');
        if (hash != std::string::npos) line.resize(hash);
        if (line.find_first_not_of(" \t") == std::string::npos) continue;

        const std::vector<std::string> tok = splitWhitespace(line);
        if (tok.empty()) continue;

        if (tok[0] == "v") {
            // 3 coords; or 4 = standard w (ignored); or 7 = x y z r g b
            // (Blone color extension). Anything else is malformed.
            if (tok.size() != 4 && tok.size() != 5 && tok.size() != 7) {
                return reject(lineNo, "v espera 3, 4 (w) ou 7 (r g b) valores", err);
            }
            Vec3 p{};
            if (!parseStrictFloat(tok[1], p.x) || !parseStrictFloat(tok[2], p.y) ||
                !parseStrictFloat(tok[3], p.z)) {
                return reject(lineNo, "coordenada de v n\u00e3o num\u00e9rica ou NaN/Inf", err);
            }
            st.pos.push_back(p);
            // Entry cap (P1): reject beyond the engine ceiling DURING parse.
            if (st.pos.size() > kMaxListEntries) {
                return rejectCap(lineNo, "v (posições)", st.pos.size(), err);
            }
            if (tok.size() == 7) {
                Vec3 c{};
                if (!parseStrictFloat(tok[4], c.x) ||
                    !parseStrictFloat(tok[5], c.y) ||
                    !parseStrictFloat(tok[6], c.z)) {
                    return reject(lineNo, "cor de v (r g b) n\u00e3o num\u00e9rica ou NaN/Inf", err);
                }
                st.color.push_back(c);
            } else {
                st.color.push_back(Vec3{1.0f, 1.0f, 1.0f}); // default white
            }
        } else if (tok[0] == "vt") {
            if (tok.size() < 2 || tok.size() > 4) {
                return reject(lineNo, "vt espera 1 a 3 valores", err);
            }
            Vec2 t{};
            if (!parseStrictFloat(tok[1], t.x)) {
                return reject(lineNo, "u de vt n\u00e3o num\u00e9rico ou NaN/Inf", err);
            }
            if (tok.size() >= 3 && !parseStrictFloat(tok[2], t.y)) {
                return reject(lineNo, "v de vt n\u00e3o num\u00e9rico ou NaN/Inf", err);
            }
            st.uv.push_back(t);
            if (st.uv.size() > kMaxListEntries) {
                return rejectCap(lineNo, "vt (uv)", st.uv.size(), err);
            }
        } else if (tok[0] == "vn") {
            if (tok.size() != 4) return reject(lineNo, "vn espera 3 valores", err);
            Vec3 n{};
            if (!parseStrictFloat(tok[1], n.x) || !parseStrictFloat(tok[2], n.y) ||
                !parseStrictFloat(tok[3], n.z)) {
                return reject(lineNo, "normal de vn n\u00e3o num\u00e9rica ou NaN/Inf", err);
            }
            st.nrm.push_back(n);
            if (st.nrm.size() > kMaxListEntries) {
                return rejectCap(lineNo, "vn (normais)", st.nrm.size(), err);
            }
        } else if (tok[0] == "f") {
            if (tok.size() < 4) {
                return reject(lineNo, "f precisa de pelo menos 3 cantos", err);
            }
            std::vector<int> loop;
            loop.reserve(tok.size() - 1);
            for (size_t i = 1; i < tok.size(); ++i) {
                // corner: v[/vt[/vn]] — an empty field means the attribute
                // is explicitly absent (e.g. "1//3"); "v", "v/vt" and
                // "v/vt/vn" all resolve the same way.
                long long p = 0, t = kAbsent, n = kAbsent;
                int part = 0;
                size_t start = 0;
                bool bad = false;
                for (size_t c = 0; c <= tok[i].size() && !bad; ++c) {
                    if (c != tok[i].size() && tok[i][c] != '/') continue;
                    const std::string field = tok[i].substr(start, c - start);
                    if (part == 0) {
                        bad = field.empty() || !parseIndexToken(field, p);
                    } else if (part == 1) {
                        bad = !field.empty() && !parseIndexToken(field, t);
                    } else if (part == 2) {
                        bad = !field.empty() && !parseIndexToken(field, n);
                    } else {
                        bad = true; // more than 3 fields
                    }
                    ++part;
                    start = c + 1;
                }
                if (bad) {
                    return reject(lineNo,
                                  "canto de face mal formado (espera v, v/vt, v//vn ou v/vt/vn)",
                                  err);
                }
                const int id = expandCorner(st, p, t, n);
                if (id < 0) {
                    return reject(lineNo, "\u00edndice de face fora do intervalo ou refer\u00eancia para frente", err);
                }
                loop.push_back(id);
            }
            st.out.faceIndices.push_back(std::move(loop));
            if (st.out.faceIndices.size() > kMaxListEntries) {
                return rejectCap(lineNo, "f (faces)", st.out.faceIndices.size(),
                                 err);
            }
        }
        // Any other directive: ignored (documented in the header).
    }

    if (st.out.positions.empty() || st.out.faceIndices.empty()) {
        if (err) {
            *err = "OBJ sem geometria utiliz\u00e1vel (sem v ou sem f)";
        }
        return std::nullopt; // geometry-less file: nothing to build
    }
    if (!st.anyNormal) st.out.normals.clear(); // absent, not placeholders
    if (!st.anyUv) st.out.uvs.clear();
    return std::move(st.out);
}

std::optional<RawMeshData> ObjLoader::load(const std::string& path,
                                           std::string* err) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        if (err) *err = "n\u00e3o foi poss\u00edvel abrir o ficheiro";
        return std::nullopt;
    }
    return parseFile(in, err);
}

} // namespace blone::io
