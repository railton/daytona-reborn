// Headless investigation controls, separate from the launcher's enhancements.
#pragma once


namespace rt {
struct EnhanceDiagnostics {
    // With positive draw distance: reorder the original list by rings,
    // retaining every entry, to distinguish membership from ordering.
    static inline bool draw_order_only = false;
};
} // namespace rt
