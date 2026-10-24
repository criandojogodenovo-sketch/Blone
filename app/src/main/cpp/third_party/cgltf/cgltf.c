/* cgltf v1.14 — official single-file glTF 2.0 parser (jkuhlmann/cgltf).
 *
 * Since v1.14 the upstream distribution is SINGLE-HEADER: the whole
 * implementation lives inside cgltf.h behind CGLTF_IMPLEMENTATION. This
 * translation unit is the canonical one-TU instantiation of that header
 * (the same pattern stb_* uses) — it exists so the build keeps one plain
 * .c source, exactly as the Fase 4 build recipe expects:
 *   third_party/cgltf/cgltf.c
 *
 * The header itself is the UNMODIFIED upstream v1.14 file.
 */
#define CGLTF_IMPLEMENTATION
#include "cgltf.h"
