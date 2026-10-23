// gen_mesh — CLI tool (missão F4-LIMITS, P8): generates OBJ/GLB test files
// of N triangles into tmp (binaries are NEVER committed).
//   gen_mesh <triangles> <out.obj|out.glb>
#include "gen_mesh.hpp"

#include <cstdio>
#include <string>

int main(int argc, char** argv) {
    if (argc != 3) {
        std::printf("uso: gen_mesh <triangulos> <saida.obj|saida.glb>\n");
        return 2;
    }
    const unsigned long long n = std::strtoull(argv[1], nullptr, 10);
    const std::string path = argv[2];
    blone::io::RawMeshData raw;
    blone::gen::generateTriangles(static_cast<size_t>(n), raw);
    bool ok;
    if (path.size() >= 4 && path.substr(path.size() - 4) == ".glb") {
        ok = blone::gen::writeGlb(raw, path);
    } else {
        ok = blone::gen::writeObj(raw, path);
    }
    std::printf("%s: %zu triangulos -> %s\n", ok ? "OK" : "FALHOU", n,
                path.c_str());
    return ok ? 0 : 1;
}
