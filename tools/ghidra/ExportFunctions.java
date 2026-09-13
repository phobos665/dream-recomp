// Post-analysis script: write every function (address, size, name, name source) as JSON.
// Usage (headless): -postScript ExportFunctions.java <output.json>
//@category Dreamcast
import java.io.FileWriter;
import java.io.PrintWriter;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.AddressSetView;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.FunctionIterator;

public class ExportFunctions extends GhidraScript {
    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        String out = args.length > 0 ? args[0] : "functions_ghidra.json";
        FunctionIterator it = currentProgram.getFunctionManager().getFunctions(true);
        long codeBytes = 0;
        int count = 0, named = 0;
        try (PrintWriter w = new PrintWriter(new FileWriter(out))) {
            w.println("{");
            w.println("  \"program\": \"" + currentProgram.getName() + "\",");
            w.println("  \"language\": \"" + currentProgram.getLanguageID() + "\",");
            w.println("  \"image_base\": \"0x" + currentProgram.getImageBase() + "\",");
            w.println("  \"functions\": [");
            boolean first = true;
            while (it.hasNext()) {
                Function f = it.next();
                AddressSetView body = f.getBody();
                long size = body.getNumAddresses();
                codeBytes += size;
                count++;
                String src = f.getSymbol().getSource().toString();
                boolean isDefault = f.getSymbol().getSource() == ghidra.program.model.symbol.SourceType.DEFAULT;
                if (!isDefault) named++;
                if (!first) w.println(",");
                first = false;
                w.print("    {\"address\": \"0x" + f.getEntryPoint() + "\", \"size\": " + size
                        + ", \"name\": \"" + f.getName().replace("\"", "\\\"") + "\", \"source\": \"" + src + "\"}");
            }
            w.println();
            w.println("  ],");
            w.println("  \"function_count\": " + count + ",");
            w.println("  \"named_count\": " + named + ",");
            w.println("  \"code_bytes\": " + codeBytes + ",");
            w.println("  \"image_bytes\": " + currentProgram.getMemory().getSize());
            w.println("}");
        }
        println("ExportFunctions: " + count + " functions (" + named + " named), " + codeBytes
                + " code bytes of " + currentProgram.getMemory().getSize() + " -> " + out);
    }
}
