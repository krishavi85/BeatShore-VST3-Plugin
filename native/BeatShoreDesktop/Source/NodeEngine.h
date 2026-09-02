#pragma once
// Node-specific wrapper over ChildProcessEngine.h, which now owns the real
// mechanism (spawns any child process, exposes its stdin/stdout as a
// deadline-bound, cancellable line interface via OverlappedPipeIO). This
// class exists only to keep the original public API -- start(nodeExe,
// scriptPath), a plain two-argument call -- so main.cpp and the existing
// NodeEngineTest need ZERO changes: same behavior, same command line built
// ("<exe>" "<script>"), same everything, just built on the shared
// mechanism instead of duplicating it. See ChildProcessEngine.h's own
// header comment for the full story, including a real pipe-naming bug
// (collision across concurrent instances in the same process) found and
// fixed while doing this generalization.
#include "ChildProcessEngine.h"

class NodeEngine : public ChildProcessEngine
{
public:
    // nodeExe: path to node.exe. scriptPath: path to analyze.js.
    bool start(const std::string& nodeExe, const std::string& scriptPath)
    {
        return ChildProcessEngine::start(nodeExe, { scriptPath }, "Node");
    }
};
