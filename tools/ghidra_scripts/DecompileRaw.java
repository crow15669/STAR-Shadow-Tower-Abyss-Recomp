// Raw image (no functions): for each address, scans back for a stack prologue (addiu sp,sp,-N), makes a function
// there and prints its decompiled C. Headless: -postScript DecompileRaw.java <out file> 0x25318 [...]
// @category STA
import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import java.io.PrintWriter;

public class DecompileRaw extends GhidraScript {
    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        DecompInterface decomp = new DecompInterface();
        decomp.openProgram(currentProgram);
        try (PrintWriter out = new PrintWriter(args[0])) {
            for (int i = 1; i < args.length; i++) {
                Address addr = toAddr(Long.decode(args[i]));
                Address start = addr;
                for (int back = 0; back < 0x1000; back += 4) {
                    Address a = addr.subtract(back);
                    int w = getInt(a);
                    if ((w & 0xFFFF8000) == 0x27BD8000) { start = a; break; }
                }
                disassemble(start);
                Function f = getFunctionAt(start);
                if (f == null) f = createFunction(start, null);
                if (f == null) { out.println("// " + args[i] + ": no function"); continue; }
                DecompileResults res = decomp.decompileFunction(f, 60, monitor);
                out.println("// ===== " + args[i] + " in function @ " + start + " =====");
                out.println(res.decompileCompleted() ? res.getDecompiledFunction().getC() : "// failed: " + res.getErrorMessage());
            }
        }
        decomp.dispose();
    }
}
