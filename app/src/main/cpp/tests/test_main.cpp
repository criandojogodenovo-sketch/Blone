// Blone core test runner (host). Returns non-zero when any check fails.
#include "test_util.h"

namespace blone::test {
void runHalfEdgeTests();
void runLoopCutTests();
void runOpsTests();
void runTransformMathTests();
void runHistoryProjectTests();
void runRenderGizmoTests();
void runSelectionStabilityTests();
void runUiModeTests();
void runBrushTests();
void runIoTests();
void runUvTests();
void runReadModeTests();
void runBigMeshTests();
} // namespace blone::test

int main() {
    using namespace blone::test;
    std::printf("== Blone core (C++) — testes do núcleo ==\n");
    runHalfEdgeTests();
    runLoopCutTests();
    runOpsTests();
    runTransformMathTests();
    runHistoryProjectTests();
    runRenderGizmoTests();
    runSelectionStabilityTests();
    runUiModeTests();
    runBrushTests();
    runIoTests();
    runUvTests(); // Fase 5.1 (UVs por canto)
    runReadModeTests(); // F4-LIMITS P3
    runBigMeshTests();  // F4-LIMITS P8 (malhas grandes)
    std::printf("\nVerificações: %d | Falhas: %d\n", g_checks, g_failures);
    if (g_failures == 0) {
        std::printf("RESULTADO: TODOS OS TESTES PASSARAM\n");
        return 0;
    }
    std::printf("RESULTADO: FALHAS DETETADAS\n");
    return 1;
}
