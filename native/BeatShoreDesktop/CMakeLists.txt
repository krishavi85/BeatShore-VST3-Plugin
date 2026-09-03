cmake_minimum_required(VERSION 3.20)
project(BeatShoreDesktop)

# Plain Win32 console app -- no JUCE dependency. It doesn't touch audio
# devices or a GUI, just a named pipe, shared memory, and a child process,
# so there's nothing JUCE would add here beyond compile time.
add_executable(BeatShoreDesktop
    Source/main.cpp
    # Embeds the real BeatShore icon (see assets/icon/generate_icon.py --
    # derived from the actual brand logo, not a placeholder) into the exe
    # itself, which is what Explorer/taskbar/Add-Remove-Programs actually
    # show; the tray icon at runtime loads this same embedded resource by
    # ID (main.cpp) rather than the generic system icon it used before.
    Source/resources.rc
)

target_compile_features(BeatShoreDesktop PRIVATE cxx_std_17)

if (MSVC)
    target_compile_options(BeatShoreDesktop PRIVATE /W4)

    # Windows long-path awareness -- a real, previously-unknown bug this
    # session found and root-caused (see STATUS.md's most recent
    # section): torch's own DLL loader hit a genuine MAX_PATH (260-char)
    # limit loading torch\lib\c10.dll from an unusually deep install
    # path. Merged into the linker's own auto-generated manifest rather
    # than replacing it (which would silently drop the UAC/common-
    # controls settings CMake already provides) -- see
    # Source/BeatShoreDesktop.manifest's own comment for the fragment
    # and why it's needed. Cheap defense-in-depth: a no-op on a Windows
    # version or policy state where it doesn't apply, not a risk.
    target_link_options(BeatShoreDesktop PRIVATE
        "/MANIFESTINPUT:${CMAKE_CURRENT_SOURCE_DIR}/Source/BeatShoreDesktop.manifest")
endif()

# Shell32 for Shell_NotifyIconA (the system tray icon), User32 for the
# window/message-loop APIs the tray icon needs, and Advapi32 for the named
# pipe's security-descriptor APIs (ConvertStringSecurityDescriptorTo...)
# -- none of these are linked by MSVC's implicit defaults the way
# kernel32 is.
target_link_libraries(BeatShoreDesktop PRIVATE Shell32 User32 Advapi32)
