#pragma once
// Python-specific wrapper over ChildProcessEngine.h -- the counterpart to
// NodeEngine.h, proving the SAME child-process/IPC mechanism genuinely
// works for a second interpreter, not just Node. Written as part of
// scoping the runtime a future real ML backend (DAC/EnCodec/MT3/etc --
// see STATUS.md) would need, per the user's explicit request to build
// this prerequisite before picking a specific model.
//
// Two genuinely non-obvious, platform-real details this class exists to
// get right, both confirmed by actually hitting them, not assumed:
//
// 1. Python fully buffers stdout by default whenever it isn't connected
//    to a real terminal -- which a redirected pipe never is. Left alone,
//    a script's print() calls would sit in an internal buffer and never
//    actually reach this process until the buffer filled or the script
//    exited, silently breaking real-time line-by-line request/response
//    IPC (every read would time out, not error). Fixed with the `-u`
//    flag (equivalent to PYTHONUNBUFFERED=1): forces stdin/stdout/stderr
//    fully unbuffered for the whole process.
//
// 2. MSVC's console defaults to the system codepage (not UTF-8) for a
//    child process's stdio -- confirmed directly: a real end-to-end MT3
//    run crashed with a genuine UnicodeEncodeError from a library's own
//    status print (a Unicode checkmark character) hitting that default
//    codepage. `-u` alone doesn't fix this -- it controls buffering, not
//    encoding. Fixed with PYTHONUTF8=1, passed via ChildProcessEngine's
//    extraEnv (added specifically to support this) rather than left for
//    every engine script to work around with its own stdout-redirect
//    tricks (which several of them ALSO do now, as defense in depth --
//    see e.g. mt3_engine.py's own header comment -- but this is the fix
//    that actually addresses the root cause).
//
// 3. A relocatable/embeddable Python's `import site` (needed so a
//    bundled site-packages' own *.pth files, e.g. distutils-precedence,
//    still get processed) ALSO processes this machine's own PER-USER
//    site-packages (%APPDATA%\Python\Python3XX\site-packages) by
//    default -- confirmed directly, not theoretical: on the actual dev
//    machine this was built on, that directory held *.pth files from a
//    completely unrelated project's editable installs, and they showed
//    up on sys.path of a supposedly self-contained, relocatable MT3
//    Model Pack the moment it was tested. This is exactly the kind of
//    "depends on user caches" contamination a redistributed runtime
//    must never have -- fixed with PYTHONNOUSERSITE=1, verified
//    directly to produce a clean sys.path containing only the bundled
//    zip/base/site-packages entries and nothing from the user profile.
#include "ChildProcessEngine.h"

class PythonEngine : public ChildProcessEngine
{
public:
    // pythonExe: path to python.exe (a bundled/embeddable interpreter in
    // a real deployment -- this class doesn't care which, same as
    // NodeEngine doesn't care whether nodeExe is a system or bundled
    // node.exe). scriptPath: the engine script to run. extraEnv: appended
    // after the vars every Python engine always gets (PYTHONUTF8=1,
    // PYTHONNOUSERSITE=1; see the class comment above for why) -- used by
    // MT3 specifically to pass MT3_CHECKPOINT_DIR/HF_HUB_OFFLINE/
    // TRANSFORMERS_OFFLINE (see main.cpp's runMt3Worker), which
    // DAC/EnCodec have no use for.
    bool start(const std::string& pythonExe, const std::string& scriptPath, const std::vector<std::string>& extraEnv = {})
    {
        std::vector<std::string> env = { "PYTHONUTF8=1", "PYTHONNOUSERSITE=1" };
        env.insert(env.end(), extraEnv.begin(), extraEnv.end());
        return ChildProcessEngine::start(pythonExe, { "-u", scriptPath }, "Python", env);
    }
};
