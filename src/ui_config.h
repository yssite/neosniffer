#pragma once
#include <windows.h>
#include <string>
#include "core.h"

namespace ss::ui {

struct ShellIntegrationStatus {
    bool applied = false;
    bool enabled = false;
    bool ok = true;
    LONG errorCode = 0;
    std::wstring detail;
};

ShellIntegrationStatus ApplyShellIntegration(bool enable);
const ShellIntegrationStatus& GetShellIntegrationStatus();
bool IsShellIntegrationRegistered();

bool ShowConfigDialog(HWND owner, ss::AppConfig& cfg, int dpi, bool darkMode);

}
