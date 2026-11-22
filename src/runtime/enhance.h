// Enhancements that act inside the recompiled game code, through hooks the
// recompiler places (m2recomp --hooks, seeds/daytona93_hooks.txt): just before
// the instruction at a hook's address, the generated code calls
// rt::hook_<name>(c). All off by default; while off a hook returns at once,
// so the game runs exactly as recompiled (rules.md: parity runs that way).
#pragma once

#include "runtime/cpu.h"

namespace rt {

struct Enhance {
    // Draw distance (see hook_draw_list): 0 = the game's own; -1 one cell
    // around the car, -2 the car's cell only; +1 the whole 5x5, +2 7x7.
    static constexpr int kDrawMin = -2, kDrawMax = 2;
    static inline int draw_distance = 0;
};

// 0x17078 (daytona93; 0x17508 in Revision A, the same code and RAM), after
// the game has built its draw list of course cells: the count
// at 0x5016c0 and the cell numbers from 0x5016c1, chosen from the 5x5 cells
// around the car's (r8) in a grid 16 cells wide (cell = x + 16 y).
void hook_draw_list(Cpu &c);
// Diagnostic observers at the game's cost updates and budget comparisons.
// Never alter registers, RAM, conditions or control flow.
void hook_scenery_cost(Cpu &c);
void hook_scenery_budget_check(Cpu &c);
void hook_mobile_slot_reset(Cpu &c);
// 0x9658 (Revision A), in the routine that lists the cars to draw: the TGP
// has returned the car's view position (0x1a1a); g0 = int(|x|) - 5 and r10 =
// int(z), and "cmpible r10,g0" drops the car when it lies outside the
// original 496-wide view cone. Widescreen (local play build): g0 is scaled
// by 496 / width so cars stay listed across the whole wider view instead of
// vanishing at the sides when overtaken. Off with M2_WIDE_CAR_CULL=0.
void hook_car_view_cone(Cpu &c);
// Keep the race identical with the wider car list: 0x95a0 (list start)
// resets the count of cars listed only thanks to the wider cone, 0x96b8 counts
// those given a level of detail, 0x8400 removes them from the drawn-car count
// the AI reads (0x5014f7). On by default with the cone; M2_WIDE_CAR_CULL=0 off.
void hook_car_list_begin(Cpu &c);
// Diagnostic, 0x17c18 (scenery object loop, Revision A): r11 = the object's
// list word, r12 and r13 = two words sent before it, r14 = its cost.
// M2_SCENERY_OBJECT_TRACE=FILE logs them (frame, cell list entry order).
void hook_scenery_object(Cpu &c);
void hook_car_list_lod(Cpu &c);
void hook_car_count_store(Cpu &c);
// 0x9490 (Revision A), "subi r3,g0,g0" with r3 = 0x7d0: the routine at 0x942c
// sums the polygon cost of the listed cars by model (near 341, mid 253, far
// 161, farthest 16) and, above 2000, demotes cars one model at a time. With
// many cars close, cars switched model from one frame to the next and the mid
// body's white rear glass flashed on them (seen in play, 22/11). M2_CAR_LOD_BUDGET=1
// lifts the allowance so each car keeps the model of its distance. Drawing
// only: the listed and drawn-car counts are unchanged.
void hook_car_lod_budget(Cpu &c);
// M2_CAR_NEAR_MODEL=1 (local play build, 22/11: "s'il y a deux modèles,
// il n'y a qu'à en garder un"): every listed car gets the near model. 0x96b8
// stores the level (r9) per car; 0x96fc stores the counts per level (r12..r15
// for levels 1..4), which 0x942c turns into per-model quotas: all go to level
// 4, and the budget is lifted. A listed car stays listed.
void hook_car_lod_counts(Cpu &c);

} // namespace rt
