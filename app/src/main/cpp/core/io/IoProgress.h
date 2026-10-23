#pragma once
// core/io — IMPORT PROGRESS + CANCELLATION (missão F4-LIMITS, P4).
//
// Anti-crash contract for big imports:
//  - PRE-COUNT before allocating: OBJ gets a counting pass over the text
//    (line-directive counts), glTF/GLB reads the counts straight from the
//    parsed JSON (accessor counts) — a file above the HARD ceiling is
//    rejected BEFORE any big allocation can happen;
//  - PROGRESS is an atomic 0..1000 slot the Kotlin dialog polls (map to
//    % by dividing by 10);
//  - CANCELLATION is an atomic flag checked every chunk (4096 lines /
//    64k vertices): a cancelled import unwinds cleanly (all-or-nothing —
//    nothing ever reaches the scene) and reports "cancelada";
//  - std::bad_alloc is caught at the worker boundary (native-lib) and
//    reported as IMPORT_FAIL "memória insuficiente".
//
// The struct is passed as an OPTIONAL pointer everywhere: the default
// nullptr keeps every existing signature and caller working (tests pay
// nothing; R3).

#include <atomic>

namespace blone::io {

struct IoProgress {
    std::atomic<int> progress{0}; // 0..1000
    std::atomic<bool> cancel{false};

    void set(int p) {
        // Monotonic clamp — the dialog never shows a step backwards.
        int cur = progress.load(std::memory_order_relaxed);
        while (p > cur &&
               !progress.compare_exchange_weak(cur, p,
                                               std::memory_order_relaxed)) {
        }
    }
    int get() const { return progress.load(std::memory_order_relaxed); }
    void requestCancel() { cancel.store(true, std::memory_order_relaxed); }
    bool cancelled() const { return cancel.load(std::memory_order_relaxed); }
    void reset() {
        progress.store(0, std::memory_order_relaxed);
        cancel.store(false, std::memory_order_relaxed);
    }
};

// Cancel check granularity (a cancelled import unwinds within ~this many
// processed units — lines, vertices or faces).
constexpr int kCancelCheckInterval = 4096;
constexpr int kProgressCheckInterval = 65536;

} // namespace blone::io
