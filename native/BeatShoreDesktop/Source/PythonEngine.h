#pragma once
// Python-specific wrapper over ChildProcessEngine.h -- the counterpart to
// NodeEngine.h, proving the SAME child-process/IPC mechanism genuinely
// works for a second interpreter, not just Node. Written as part of
// scoping the runtime a future real ML backend (DAC/EnCodec/MT3/etc --
// see STATUS.md) would need, per the user's explicit request to build
// this prerequisite before picking a specific model.
//
// The one genuinely non-obvious, platform-real detail this class exists
// to get right: Python fully buffers stdout by default whenever it isn't
// connected to a real terminal -- which a redirected pipe never is. Left
// alone, a script's print() calls would sit in an internal buffer and
// never actually reach this process until the buffer filled or the
// script exited, silently breaking real-time line-by-line request/
// response IPC (every read would time out, not error -- easy to
// misdiagnose as "the pipe mechanism doesn't work for Python" when the
// actual cause is Python's own I/O buffering policy). The standard,
// documented fix is the `-u` flag (equivalent to setting
// PYTHONUNBUFFERED=1): forces stdin/stdout/stderr fully unbuffered for
// the whole process. Confirmed necessary by direct testing, not assumed
// -- see python_engine_test.cpp's own header comment.
#include "ChildProcessEngine.h"

class PythonEngine : public ChildProcessEngine
{
public:
    // pythonExe: path to python.exe (a bundled/embeddable interpreter in
    // a real deployment -- this class doesn't care which, same as
    // NodeEngine doesn't care whether nodeExe is a system or bundled
    // node.exe). scriptPath: the engine script to run.
    bool start(const std::string& pythonExe, const std::string& scriptPath)
    {
        return ChildProcessEngine::start(pythonExe, { "-u", scriptPath }, "Python");
    }
};
