// Lists the instructions that hold one of the given constants (as any scalar operand), with their functions.
// Headless (read-only, -noanalysis): -postScript FindImm.java <out file> 0x159 [0x... ...]
// @category STA
import ghidra.app.script.GhidraScript;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.Instruction;
import ghidra.program.model.listing.InstructionIterator;
import ghidra.program.model.scalar.Scalar;
import java.io.PrintWriter;
import java.util.HashSet;
import java.util.Set;

public class FindImm extends GhidraScript {
    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        Set<Long> wanted = new HashSet<>();
        for (int i = 1; i < args.length; i++)
            wanted.add(Long.decode(args[i]));
        try (PrintWriter out = new PrintWriter(args[0])) {
            InstructionIterator it = currentProgram.getListing().getInstructions(true);
            while (it.hasNext()) {
                Instruction ins = it.next();
                for (int op = 0; op < ins.getNumOperands(); op++) {
                    for (Object o : ins.getOpObjects(op)) {
                        if (o instanceof Scalar) {
                            long v = ((Scalar) o).getUnsignedValue();
                            long s = ((Scalar) o).getSignedValue();
                            if (wanted.contains(v) || wanted.contains(s)) {
                                Function f = getFunctionContaining(ins.getAddress());
                                out.println(ins.getAddress() + " " + (f == null ? "?" : f.getName()) + " : " + ins.toString());
                            }
                        }
                    }
                }
            }
        }
    }
}
