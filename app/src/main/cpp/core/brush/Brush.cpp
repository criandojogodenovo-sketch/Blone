#include "Brush.h"

#include <algorithm>
#include <cmath>

namespace blone::brush {

namespace {

// World units displaced per application at strength 1.0 and falloff 1.0.
// Stroke move events arrive at touch-rate (~60 Hz); with the default
// strength 0.5 a one-second hold in the same spot yields ~1.5 world units —
// a visible but controllable deformation on the phase-2 primitives.
// Tuning constant, documented in ARCHITECTURE.md; pinned to be POSITIVE by
// tests (sculpt must actually move vertices).
constexpr float kBrushStepWorld = 0.05f;

// Safe parameter ranges (UI sliders are clamped to the same ranges).
constexpr float kMinRadius = 0.02f;
constexpr float kMaxRadius = 4.0f;
constexpr float kMinStrength = 0.01f;
constexpr float kMaxStrength = 1.0f;
constexpr float kMinOpacity = 0.01f;
constexpr float kMaxOpacity = 1.0f;

inline float clampf(float v, float lo, float hi) {
    return std::min(hi, std::max(lo, v));
}

inline bool finite3(float x, float y, float z) {
    return std::isfinite(x) && std::isfinite(y) && std::isfinite(z);
}

} // namespace

bool settingsValid(const BrushSettings& s) {
    if (!std::isfinite(s.radius) || s.radius < kMinRadius || s.radius > kMaxRadius) {
        return false;
    }
    if (!std::isfinite(s.strength) || s.strength < kMinStrength ||
        s.strength > kMaxStrength) {
        return false;
    }
    if (!std::isfinite(s.opacity) || s.opacity < kMinOpacity ||
        s.opacity > kMaxOpacity) {
        return false;
    }
    if (!finite3(s.color.x, s.color.y, s.color.z)) return false;
    if (s.color.x < 0.0f || s.color.x > 1.0f || s.color.y < 0.0f ||
        s.color.y > 1.0f || s.color.z < 0.0f || s.color.z > 1.0f) {
        return false;
    }
    switch (s.mode) {
        case SculptMode::Raise:
        case SculptMode::Dig:
            break;
        default:
            return false;
    }
    return true;
}

BrushSettings sanitized(const BrushSettings& s) {
    BrushSettings out = s;
    out.radius = std::isfinite(s.radius)
                     ? clampf(s.radius, kMinRadius, kMaxRadius)
                     : BrushSettings{}.radius;
    out.strength = std::isfinite(s.strength)
                       ? clampf(s.strength, kMinStrength, kMaxStrength)
                       : BrushSettings{}.strength;
    out.opacity = std::isfinite(s.opacity)
                      ? clampf(s.opacity, kMinOpacity, kMaxOpacity)
                      : BrushSettings{}.opacity;
    out.color = Vec3{std::isfinite(s.color.x) ? clampf(s.color.x, 0.0f, 1.0f)
                                              : kDefaultVertexColor.x,
                     std::isfinite(s.color.y) ? clampf(s.color.y, 0.0f, 1.0f)
                                              : kDefaultVertexColor.y,
                     std::isfinite(s.color.z) ? clampf(s.color.z, 0.0f, 1.0f)
                                              : kDefaultVertexColor.z};
    switch (s.mode) {
        case SculptMode::Raise:
        case SculptMode::Dig:
            out.mode = s.mode;
            break;
        default:
            out.mode = SculptMode::Raise;
            break;
    }
    return out;
}

float falloff(float dist, float radius) {
    if (!std::isfinite(dist) || !std::isfinite(radius) || radius <= 0.0f) {
        return 0.0f;
    }
    if (dist >= radius) return 0.0f;
    if (dist <= 0.0f) return 1.0f;
    // Smooth kernel (cosine): C1-continuous at both ends, cheap, no texture
    // lookups. t=0 -> 1, t=1 -> 0.
    const float t = dist / radius;
    return 0.5f * (1.0f + std::cos(t * static_cast<float>(M_PI)));
}

void collectAffected(const HalfEdgeMesh& m, const Vec3& hitLocal, float radius,
                     std::vector<ElemId>& out) {
    out.clear();
    if (!finite3(hitLocal.x, hitLocal.y, hitLocal.z)) return;
    if (!std::isfinite(radius) || radius <= 0.0f) return;
    const float r2 = radius * radius;
    for (const auto& v : m.vertices()) {
        if (!v.alive) continue;
        const float dx = v.pos.x - hitLocal.x;
        const float dy = v.pos.y - hitLocal.y;
        const float dz = v.pos.z - hitLocal.z;
        const float d2 = dx * dx + dy * dy + dz * dz;
        if (d2 <= r2) out.push_back(v.id);
    }
}

bool sculptApply(HalfEdgeMesh& m, const std::vector<ElemId>& verts,
                 const Vec3& hitLocal, const Vec3& hitNormal,
                 const BrushSettings& rawSettings) {
    if (!finite3(hitLocal.x, hitLocal.y, hitLocal.z)) return false;
    if (!finite3(hitNormal.x, hitNormal.y, hitNormal.z)) return false;
    const Vec3 dirRaw = normalize(hitNormal);
    // Degenerate normal (zero-area face / garbage): reject — displacing along
    // an undefined direction is never acceptable.
    if (length(dirRaw) < 0.5f) return false;
    const BrushSettings s = sanitized(rawSettings);
    if (verts.empty()) return true; // nothing to do is NOT an error

    // Capture previous positions (revert path) and apply with falloff.
    std::vector<Vec3> prev;
    prev.reserve(verts.size());
    for (const ElemId v : verts) {
        if (!m.vertexAlive(v)) continue;
        prev.push_back(m.vrt(v).pos);
    }

    const float dirSign = (s.mode == SculptMode::Dig) ? -1.0f : 1.0f;
    for (const ElemId v : verts) {
        if (!m.vertexAlive(v)) continue;
        const Vec3 p = m.vrt(v).pos;
        const float dist = length(p - hitLocal);
        const float w = falloff(dist, s.radius);
        if (w <= 0.0f) continue;
        const float step = kBrushStepWorld * s.strength * w * dirSign;
        m.vrt(v).pos = p + dirRaw * step;
    }

    // Non-finite protection: ANY non-finite result reverts the whole
    // application (mesh intact — the same rollback contract as SceneOps).
    bool finite = true;
    for (const ElemId v : verts) {
        if (!m.vertexAlive(v)) continue;
        const Vec3& p = m.vrt(v).pos;
        if (!finite3(p.x, p.y, p.z)) {
            finite = false;
            break;
        }
    }
    if (!finite) {
        size_t i = 0;
        for (const ElemId v : verts) {
            if (!m.vertexAlive(v)) continue;
            m.vrt(v).pos = prev[i];
            ++i;
        }
        return false;
    }
    return true;
}

bool paintApply(HalfEdgeMesh& m, const std::vector<ElemId>& verts,
                const Vec3& hitLocal, const BrushSettings& rawSettings) {
    if (!finite3(hitLocal.x, hitLocal.y, hitLocal.z)) return false;
    const BrushSettings s = sanitized(rawSettings);
    if (verts.empty()) return true;

    std::vector<Vec3> prev;
    prev.reserve(verts.size());
    for (const ElemId v : verts) {
        if (!m.vertexAlive(v)) continue;
        prev.push_back(m.vrt(v).color);
    }

    for (const ElemId v : verts) {
        if (!m.vertexAlive(v)) continue;
        const Vec3 p = m.vrt(v).pos;
        const float dist = length(p - hitLocal);
        const float w = falloff(dist, s.radius);
        if (w > 0.0f) {
            const float blend = s.opacity * w;
            const Vec3& c = m.vrt(v).color;
            m.vrt(v).color = Vec3{c.x + (s.color.x - c.x) * blend,
                                  c.y + (s.color.y - c.y) * blend,
                                  c.z + (s.color.z - c.z) * blend};
        }
    }

    bool finite = true;
    for (const ElemId v : verts) {
        if (!m.vertexAlive(v)) continue;
        const Vec3& c = m.vrt(v).color;
        if (!finite3(c.x, c.y, c.z)) {
            finite = false;
            break;
        }
    }
    if (!finite) {
        size_t i = 0;
        for (const ElemId v : verts) {
            if (!m.vertexAlive(v)) continue;
            m.vrt(v).color = prev[i];
            ++i;
        }
        return false;
    }
    return true;
}

} // namespace blone::brush
