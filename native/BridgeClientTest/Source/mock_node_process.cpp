// Minimal stand-in for node.exe, used only by NodeEngineTest to drive
// controllable Node-side failure modes deterministically -- a real Node
// script that reliably hangs before its first line of output, or writes
// some progress then goes silent forever, isn't something you can trigger
// on demand. Mode is selected via argv[1], which is exactly the
// `scriptPath` argument NodeEngine::start() passes through as this
// process's own second command-line argument -- NodeEngine has no opinion
// about what that argument means, so repurposing it as a mode selector
// here doesn't require touching NodeEngine.h at all.
#include <iostream>
#include <string>
#include <thread>
#include <chrono>

int main(int argc, char** argv)
{
    std::string mode = argc > 1 ? argv[1] : "";

    if (mode == "silent")
    {
        // Never prints anything -- models "Node starts but never writes
        // READY" (e.g. hung before reaching its first line of output).
        std::this_thread::sleep_for(std::chrono::seconds(120));
        return 0;
    }

    if (mode == "progress-then-silent")
    {
        std::cout << "{\"type\":\"READY\",\"supportedKinds\":[\"tempo\"]}" << std::endl;
        std::cout << "{\"type\":\"ANALYSIS_PROGRESS\",\"requestId\":\"x\",\"progress\":0.1}" << std::endl;
        // Models "Node writes progress and then becomes silent" -- never
        // sends a terminal ANALYSIS_RESULT/MIDI_RESULT/ERROR after this.
        std::this_thread::sleep_for(std::chrono::seconds(120));
        return 0;
    }

    return 1; // unknown mode -- test bug, not a scenario being modeled
}
