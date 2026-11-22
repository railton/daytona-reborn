# Synthetic producer-IP counterexample

No Sega data. This diagnostic compiles a synthetic lda/st/st/stob sequence with
the real m2recomp in exact and --fast_inaccuracy modes, then executes both with
the real Cpu/Lockstep and a recording Bus. It does not change the game runtime.
Use an existing compatible default desktop build and the same compiler/ABI;
M2_FAST_GEN postprocessed targets are outside this fixture.

    cmake -S tests/producer_ip -B build/producer-ip -DRUNTIME_BUILD=/absolute/runtime/build
    cmake --build build/producer-ip --config Release
    ctest --test-dir build/producer-ip -C Release --output-on-failure

On Windows the validated generator is Visual Studio 17 2022, x64, ClangCL.
Build target m2recomp in the runtime build first. Generated files remain only in
the build directory. The fixture asserts values, addresses, widths, instruction
counts and observed IPs. The byte write reaches Bus at0x800001; this fixture does
not claim to test the Model2 board's later lane-mask conversion.

Exact mode observes0x1008/0x100c/0x1010 and counts1/2/3. Block mode observes
0x1000/count0 for all three stores. Both finish at count4. A subsequent direct
host bus write retains the parked CPU IP/count without executing an instruction.
Thus a CPU-state sample alone proves neither exact writer PC nor caller identity.
No persistent object IDs, complete interrupt/call semantics or MAME parity are
claimed. The next production diagnostic must label observations and execution
mode explicitly rather than infer an exact producer or object identity.

The boundary tests schedule a Lockstep callback at completed instruction 1.
Both exact builds see the callback before the first store, with the same
write values, widths and addresses. In block mode the baseline reaches its
end count without delivering the callback. A static hook at the first store
adds a block boundary where the callback runs before any write. A hook at
the second store delivers the callback after one write while still reporting
the scheduled count of 1. The callback sees the second store's IP.
The port observes the IP and count at the start of its executing block.
With the second-store hook, the first write still observes the original
block, and the last two observe the block that starts at the hook.

The static hook records only the test bus state. These controls exercise
callback ordering under the current recompiler. Interrupt delivery and
timing parity on the game need separate checks.
