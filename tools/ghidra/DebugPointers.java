// Diagnostic: report how many aligned words survive each SeedFromPointers filter stage.
//@category Dreamcast
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Listing;
import ghidra.program.model.mem.Memory;

public class DebugPointers extends GhidraScript {
    @Override
    public void run() throws Exception {
        Memory mem = currentProgram.getMemory();
        Listing lst = currentProgram.getListing();
        long lo = currentProgram.getMinAddress().getOffset();
        long hi = currentProgram.getMaxAddress().getOffset();
        long total = 0, inImage = 0, even = 0, srcInInstr = 0, tgtInstr = 0, tgtDefined = 0, tgtMid = 0, cand = 0;
        for (long off = lo; off + 4 <= hi; off += 4) {
            total++;
            Address a = toAddr(off);
            long v = mem.getInt(a) & 0xFFFFFFFFL;
            if (v < lo || v >= hi) continue;
            inImage++;
            if ((v & 1) != 0) continue;
            even++;
            if (lst.getInstructionContaining(a) != null) { srcInInstr++; continue; }
            Address t = toAddr(v);
            if (getInstructionAt(t) != null) { tgtInstr++; continue; }
            if (lst.getDefinedDataAt(t) != null) { tgtDefined++; continue; }
            if (lst.getInstructionContaining(t) != null) { tgtMid++; continue; }
            cand++;
        }
        println("DebugPointers: lo=" + Long.toHexString(lo) + " hi=" + Long.toHexString(hi) + " total=" + total
                + " inImage=" + inImage + " even=" + even + " srcInInstr=" + srcInInstr + " tgtInstr=" + tgtInstr
                + " tgtDefined=" + tgtDefined + " tgtMid=" + tgtMid + " candidates=" + cand);
    }
}
