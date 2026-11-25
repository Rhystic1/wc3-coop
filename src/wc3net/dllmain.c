#include <windows.h>

void wc3net_on_attach(HMODULE game);
void wc3net_install(HMODULE game);

static DWORD WINAPI wc3net_init(LPVOID unused)
{
    (void)unused;
    HMODULE game = GetModuleHandleA("WC3W.EXE");
    wc3net_on_attach(game);
#ifndef WC3NET_NO_HOOK
    if (game) wc3net_install(game);
#endif
    return 0;
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved)
{
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(inst);
        CreateThread(NULL, 0, wc3net_init, NULL, 0, NULL);
    }
    return TRUE;
}
