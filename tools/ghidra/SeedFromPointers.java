// Literal-pool pointer seeding for Katana binaries (ADR 8, "constant-pool harvesting").
//
// SH-4 code reaches most functions through 32-bit pointers held in literal pools
// (MOV.L @(disp,PC),Rn ; JSR @Rn) and function tables, so recursive descent from the entry point
// alone stalls after a few hundred functions. Run as a POST-analysis script: after the normal pass
// has typed what it can, every aligned 32-bit word that points at still-undefined bytes inside the
// image at an even address is treated as a code pointer. The target is disassembled and made a
// function, incremental analysis follows the new flow, and the scan repeats until a round adds
// nothing. Running after analysis (rather than before) means pointers into already-typed data
// tables are left alone, which keeps false positives down.
//
// Usage (headless): -preScript MarkEntry.java -postScript SeedFromPointers.java
//                   -postScript ExportFunctions.java out.json
//@category Dreamcast
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.mem.Memory;
import ghidra.program.model.mem.MemoryBlock;

public class SeedFromPointers extends GhidraScript {
    private static final int MAX_ROUNDS = 8;

    @Override
    public void run() throws Exception {
        Memory mem = currentProgram.getMemory();

        int totalCreated = 0;
        for (int round = 1; round <= MAX_ROUNDS; round++) {
            int candidates = 0, created = 0;
            // Walk each initialised block separately: the image and any relocated copies need not
            // be contiguous, and reading across the gap between them throws.
            for (MemoryBlock blk : mem.getBlocks()) {
                if (!blk.isInitialized()) continue;
                long loOff = blk.getStart().getOffset();
                long hiOff = blk.getEnd().getOffset();
                for (long off = loOff; off + 4 <= hiOff; off += 4) {
                    if (monitor.isCancelled()) return;
                    Address a = toAddr(off);
                    // Skip words that are themselves inside instructions: a pointer lives in data.
                    if (currentProgram.getListing().getInstructionContaining(a) != null) continue;
                    long v = mem.getInt(a) & 0xFFFFFFFFL;
                    if ((v & 1) != 0) continue;
                    Address target = toAddr(v);
                    MemoryBlock tb = mem.getBlock(target);
                    if (tb == null || !tb.isInitialized()) continue;
                    if (getInstructionAt(target) != null
                            || currentProgram.getListing().getDefinedDataAt(target) != null) continue;
                    if (currentProgram.getListing().getInstructionContaining(target) != null) continue;
                    candidates++;
                    if (!disassemble(target)) continue;
                    if (getInstructionAt(target) == null) continue;
                    if (createFunction(target, null) != null) created++;
                }
            }
            println("SeedFromPointers: round " + round + ": " + candidates + " pointer targets, "
                    + created + " functions created");
            totalCreated += created;
            if (created == 0) break;
            analyzeChanges(currentProgram);
        }
        println("SeedFromPointers: total " + totalCreated + " functions seeded, "
                + currentProgram.getFunctionManager().getFunctionCount() + " functions now");
    }
}
