// Prints the decompiled C of the functions containing the given addresses.
// Headless (read-only, -noanalysis): -postScript Decompile.java <out file> 0x117450 [0x... ...]
// @category STA
import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import java.io.PrintWriter;

public class Decompile extends GhidraScript {
    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        DecompInterface decomp = new DecompInterface();
        decomp.openProgram(currentProgram);
        try (PrintWriter out = new PrintWriter(args[0])) {
            for (int i = 1; i < args.length; i++) {
                Address addr = toAddr(Long.decode(args[i]));
                Function f = getFunctionContaining(addr);
                if (f == null) {
                    // Code Ghidra never made a function (reached only through a pointer): make one here, in memory
                    // (the project is opened read-only, so nothing is saved).
                    disassemble(addr);
                    f = createFunction(addr, null);
                }
                if (f == null) {
                    out.println("// " + args[i] + ": no function");
                    continue;
                }
                DecompileResults res = decomp.decompileFunction(f, 60, monitor);
                out.println("// ===== " + args[i] + " in " + f.getName() + " @ " + f.getEntryPoint() + " =====");
                out.println(res.decompileCompleted() ? res.getDecompiledFunction().getC() : "// failed: " + res.getErrorMessage());
            }
        }
        decomp.dispose();
    }
}
