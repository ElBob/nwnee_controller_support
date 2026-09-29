// Headless Ghidra script: decompile functions by name and write one .c file each.
// Args: <outDir> <name> [name...]. Names match either the full demangled name
// (e.g. CNWSMessage::HandlePlayerToServerInputDriveControl) or the bare name.
// @category nwpad
import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.listing.Function;

import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.nio.file.Paths;

public class DecompileFunction extends GhidraScript {
    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        if (args.length < 2) {
            println("DECOMPILE-ERROR usage: <outDir> <name>...");
            return;
        }
        Path outDir = Paths.get(args[0]);
        Files.createDirectories(outDir);
        DecompInterface dec = new DecompInterface();
        dec.openProgram(currentProgram);
        for (int i = 1; i < args.length; i++) {
            String want = args[i];
            int n = 0;
            for (Function f : currentProgram.getFunctionManager().getFunctions(true)) {
                if (!f.getName(true).equals(want) && !f.getName().equals(want)) continue;
                DecompileResults r = dec.decompileFunction(f, 180, monitor);
                String body = r.decompileCompleted() ? r.getDecompiledFunction().getC()
                                                     : "/* decompile failed: " + r.getErrorMessage() + " */";
                String header = "/* " + f.getName(true) + " @ " + f.getEntryPoint() + " */\n";
                String file = want.replaceAll("[^A-Za-z0-9_]+", "_") + (n > 0 ? "_" + n : "") + ".c";
                Files.write(outDir.resolve(file), (header + body).getBytes(StandardCharsets.UTF_8));
                println("DECOMPILE-OK " + want + " -> " + file);
                n++;
            }
            if (n == 0) println("DECOMPILE-MISSING " + want);
        }
        dec.dispose();
        println("DECOMPILE-DONE");
    }
}
