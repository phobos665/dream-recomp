// Pre-analysis script for a raw Dreamcast boot binary imported with the BinaryLoader at
// 0x8C010000: mark the load address as the entry point so auto-analysis follows control flow
// from it. Usage (headless): -preScript MarkEntry.java
//@category Dreamcast
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.symbol.SourceType;

public class MarkEntry extends GhidraScript {
    @Override
    public void run() throws Exception {
        Address entry = currentProgram.getMinAddress();
        currentProgram.getSymbolTable().addExternalEntryPoint(entry);
        createLabel(entry, "_start", true, SourceType.USER_DEFINED);
        disassemble(entry);
        createFunction(entry, "_start");
        println("MarkEntry: entry at " + entry);
    }
}
