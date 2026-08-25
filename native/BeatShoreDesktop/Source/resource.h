// Shared between resources.rc (the resource compiler) and main.cpp (the
// runtime LoadIconA call for the tray icon) so the resource ID can't
// silently drift between the two.
#pragma once

#define IDI_APP_ICON 101
