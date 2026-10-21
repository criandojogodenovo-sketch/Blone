#include "ObjLoader.h"

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
constexpr uint64_t kIdxMask = 0x1FFFFFull; // 21 bits per field in the key

struct ObjParserState {
    std::vector<Vec3> pos;
    std::vector<Vec3> nrm;
    std::vector<Vec2> uv;
    std::vector<Vec3> color; // per position (white default)

    std::unordered_map<uint64_t, int> cornerMap; // tuple -> expanded id
    bool anyNormal = false;
    bool anyUv = false;

    RawMeshData out;
};

uint64_t tupleKey(int p, int t, int n) {
    return (static_cast<uint64_t>(static_cast<uint32_t>(p)) << 42) |
           (static_cast<uint64_t>(static_cast<uint32_t>(t)) << 21) |
           static_cast<uint64_t>(static_cast<uint32_t>(n));
}

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
    if (pi < 0 || pi >= 0x1FFFFE || ti >= 0x1FFFFE || ni >= 0x1FFFFE) return -1;

    if (ni >= 0) st.anyNormal = true;
    if (ti >= 0) st.anyUv = true;

    const uint64_t key =
            tupleKey(pi, ti >= 0 ? ti : static_cast<int>(kIdxMask),
                     ni >= 0 ? ni : static_cast<int>(kIdxMask));
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

std::optional<RawMeshData> ObjLoader::parseFile(std::istream& in) {
    if (!in) return std::nullopt;

    ObjParserState st;
    std::string line;
    while (std::getline(in, line)) {
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
                return std::nullopt;
            }
            Vec3 p{};
            if (!parseStrictFloat(tok[1], p.x) || !parseStrictFloat(tok[2], p.y) ||
                !parseStrictFloat(tok[3], p.z)) {
                return std::nullopt;
            }
            st.pos.push_back(p);
            if (tok.size() == 7) {
                Vec3 c{};
                if (!parseStrictFloat(tok[4], c.x) ||
                    !parseStrictFloat(tok[5], c.y) ||
                    !parseStrictFloat(tok[6], c.z)) {
                    return std::nullopt;
                }
                st.color.push_back(c);
            } else {
                st.color.push_back(Vec3{1.0f, 1.0f, 1.0f}); // default white
            }
        } else if (tok[0] == "vt") {
            if (tok.size() < 2 || tok.size() > 4) return std::nullopt;
            Vec2 t{};
            if (!parseStrictFloat(tok[1], t.x)) return std::nullopt;
            if (tok.size() >= 3 && !parseStrictFloat(tok[2], t.y)) return std::nullopt;
            st.uv.push_back(t);
        } else if (tok[0] == "vn") {
            if (tok.size() != 4) return std::nullopt;
            Vec3 n{};
            if (!parseStrictFloat(tok[1], n.x) || !parseStrictFloat(tok[2], n.y) ||
                !parseStrictFloat(tok[3], n.z)) {
                return std::nullopt;
            }
            st.nrm.push_back(n);
        } else if (tok[0] == "f") {
            if (tok.size() < 4) return std::nullopt; // directive + 3 corners
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
                if (bad) return std::nullopt;
                const int id = expandCorner(st, p, t, n);
                if (id < 0) return std::nullopt;
                loop.push_back(id);
            }
            st.out.faceIndices.push_back(std::move(loop));
        }
        // Any other directive: ignored (documented in the header).
    }

    if (st.out.positions.empty() || st.out.faceIndices.empty()) {
        return std::nullopt; // geometry-less file: nothing to build
    }
    if (!st.anyNormal) st.out.normals.clear(); // absent, not placeholders
    if (!st.anyUv) st.out.uvs.clear();
    return std::move(st.out);
}

std::optional<RawMeshData> ObjLoader::load(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return std::nullopt;
    return parseFile(in);
}

} // namespace blone::io
