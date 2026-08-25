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
endif()

# Shell32 for Shell_NotifyIconA (the system tray icon), User32 for the
# window/message-loop APIs the tray icon needs, and Advapi32 for the named
# pipe's security-descriptor APIs (ConvertStringSecurityDescriptorTo...)
# -- none of these are linked by MSVC's implicit defaults the way
# kernel32 is.
target_link_libraries(BeatShoreDesktop PRIVATE Shell32 User32 Advapi32)
