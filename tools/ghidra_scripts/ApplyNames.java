// Names functions from a CSV (header row, then Name,Address,...): creates the function if Ghidra has none there.
// Headless: -postScript ApplyNames.java <csv>
// @category STA
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.symbol.SourceType;
import java.io.BufferedReader;
import java.io.FileReader;

public class ApplyNames extends GhidraScript {
    @Override
    public void run() throws Exception {
        String path = askString("Names CSV", "Path");
        int named = 0, created = 0, failed = 0;
        try (BufferedReader in = new BufferedReader(new FileReader(path))) {
            String line = in.readLine();  // header
            while ((line = in.readLine()) != null) {
                String[] cols = line.split(",");
                if (cols.length < 2) continue;
                Address addr = toAddr(Long.decode(cols[1].trim()));
                disassemble(addr);
                Function f = getFunctionAt(addr);
                if (f == null) {
                    f = createFunction(addr, cols[0].trim());
                    if (f == null) { failed++; println("could not create " + cols[0] + " at " + addr); continue; }
                    created++;
                } else {
                    f.setName(cols[0].trim(), SourceType.USER_DEFINED);
                }
                named++;
            }
        }
        println(String.format("ApplyNames: %d named (%d new functions), %d failed", named, created, failed));
    }
}
