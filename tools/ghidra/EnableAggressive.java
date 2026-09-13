// Pre-analysis: turn on Ghidra's aggressive instruction finder, which on the Crazy Taxi binary
// raised discovery from 283 to 1127 functions before any pointer seeding.
//@category Dreamcast
import ghidra.app.script.GhidraScript;

public class EnableAggressive extends GhidraScript {
    @Override
    public void run() throws Exception {
        setAnalysisOption(currentProgram, "Aggressive Instruction Finder", "true");
        println("EnableAggressive: on");
    }
}
