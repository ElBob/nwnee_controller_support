// Headless Ghidra pre-script: turn off non-returning function discovery.
// On nwserver-linux it wrongly marks functions such as CNWMessage::ReadFLOAT and
// tracy::GetProfiler as non-returning, which truncates every caller's decompile.
// @category nwpad
import ghidra.app.script.GhidraScript;

public class DisableNonReturning extends GhidraScript {
    @Override
    public void run() throws Exception {
        setAnalysisOption(currentProgram, "Non-Returning Functions - Discovered", "false");
        println("nwpad: disabled Non-Returning Functions - Discovered");
    }
}
