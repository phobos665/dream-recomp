// Memory layout for Crazy Taxi's 1ST_READ.BIN (measured 2026-09-11, see checklist-report.md).
//
// The image is linked at 0x0C010000, the physical (P0) alias of the 0x8C010000 load address, so it
// must be imported with -loader-baseAddr 0x0c010000. The first 0x100 bytes are a loader that copies
// [0x0C010100, 0x0C014000) to 0x8C004000 and jumps there; that stub is the Katana startup code and
// runs from the copy, so a second, initialised block at 0x8C004000 lets its own literals resolve.
// Both the loader and the stub copy are marked as entry points.
//@category Dreamcast
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.symbol.SourceType;

public class CrazyTaxiLayout extends GhidraScript {
    @Override
    public void run() throws Exception {
        Address base = toAddr(0x0c010000L);
        if (!currentProgram.getMinAddress().equals(base)) {
            printerr("CrazyTaxiLayout: expected image base 0x0c010000, got " + currentProgram.getMinAddress());
        }
        setAnalysisOption(currentProgram, "Aggressive Instruction Finder", "true");

        // Loader entry at the load address.
        currentProgram.getSymbolTable().addExternalEntryPoint(base);
        createLabel(base, "_loader", true, SourceType.USER_DEFINED);
        disassemble(base);
        createFunction(base, "_loader");

        // Startup stub as it runs after the copy.
        byte[] stub = getBytes(toAddr(0x0c010100L), 0x3f00);
        Address stubAt = toAddr(0x8c004000L);
        if (getMemoryBlock(stubAt) == null) {
            createMemoryBlock("stub_copy", stubAt, stub, false);
        }
        currentProgram.getSymbolTable().addExternalEntryPoint(stubAt);
        createLabel(stubAt, "_startup_stub", true, SourceType.USER_DEFINED);
        disassemble(stubAt);
        createFunction(stubAt, "_startup_stub");
        println("CrazyTaxiLayout: image at 0x0c010000, stub copy at 0x8c004000 (" + stub.length + " bytes)");
    }
}
