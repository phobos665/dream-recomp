// Post-analysis: for each address given as a script argument, print the containing function's
// disassembly (or 64 instructions from the address if no function contains it) to the output file
// named by the first argument. Used to inspect specific hardware-register accesses.
//@category Dreamcast
import java.io.FileWriter;
import java.io.PrintWriter;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.Instruction;
import ghidra.program.model.listing.InstructionIterator;

public class DumpFunctionsAt extends GhidraScript {
    @Override
    public void run() throws Exception {
        String[] raw = getScriptArgs();
        // Accept addresses either as separate arguments or as one whitespace-separated string
        // (zsh does not word-split unquoted variables, so callers often pass the latter).
        java.util.List<String> args = new java.util.ArrayList<>();
        for (String r : raw) {
            for (String part : r.trim().split("\\s+")) {
                if (!part.isEmpty()) args.add(part);
            }
        }
        try (PrintWriter w = new PrintWriter(new FileWriter(args.get(0)))) {
            for (int i = 1; i < args.size(); i++) {
                Address a = toAddr(Long.parseLong(args.get(i).replace("0x", ""), 16));
                Function f = getFunctionContaining(a);
                w.println("==== " + a + (f != null ? " in " + f.getName() + " @ " + f.getEntryPoint()
                        + " size " + f.getBody().getNumAddresses() : " (no function)"));
                if (f != null) {
                    InstructionIterator it = currentProgram.getListing().getInstructions(f.getBody(), true);
                    while (it.hasNext()) {
                        Instruction ins = it.next();
                        String mark = ins.getAddress().equals(a) ? " <==" : "";
                        w.println("  " + ins.getAddress() + "  " + ins.toString() + mark);
                    }
                } else {
                    Instruction ins = getInstructionAt(a);
                    if (ins == null) { disassemble(a); ins = getInstructionAt(a); }
                    for (int k = 0; k < 64 && ins != null; k++) {
                        w.println("  " + ins.getAddress() + "  " + ins.toString());
                        ins = ins.getNext();
                    }
                }
            }
        }
        println("DumpFunctionsAt: wrote " + args.get(0));
    }
}
