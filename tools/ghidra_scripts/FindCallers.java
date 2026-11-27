// Lists the references to the given addresses (calls, jumps, data pointers), with the functions they are in.
// Headless (read-only, -noanalysis): -postScript FindCallers.java <out file> 0x16dc70 [0x... ...]
// @category STA
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.symbol.Reference;
import java.io.PrintWriter;

public class FindCallers extends GhidraScript {
    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        try (PrintWriter out = new PrintWriter(args[0])) {
            for (int i = 1; i < args.length; i++) {
                Address addr = toAddr(Long.decode(args[i]));
                out.println("== " + args[i]);
                for (Reference ref : getReferencesTo(addr)) {
                    Function f = getFunctionContaining(ref.getFromAddress());
                    out.println(ref.getFromAddress() + " " + ref.getReferenceType() + " in " + (f == null ? "-" : f.getName()));
                }
            }
        }
    }
}
