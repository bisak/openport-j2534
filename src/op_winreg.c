/*
 * op_winreg.c — makes the DLL findable by J2534 applications on Windows.
 *
 * A J2534 application does not search for DLLs. It lists the keys under
 * HKEY_LOCAL_MACHINE\SOFTWARE\PassThruSupport.04.04, one per installed
 * interface, and loads the path in the key's FunctionLibrary value (SAE
 * J2534-1 section 8.1). `regsvr32 <dll>` writes that key for this DLL
 * wherever the DLL is, and `regsvr32 /u <dll>` removes it.
 *
 * A 32-bit application reads the 32-bit registry view and a 64-bit one the
 * 64-bit view. Each DLL writes HKLM\SOFTWARE as its own process sees it, so
 * the 32-bit DLL registered by the 32-bit regsvr32 lands in WOW6432Node with
 * no code here for it. Both DLLs use the same key name, one per view.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <windows.h>

#include <string.h>

#define OP_REG_KEY "SOFTWARE\\PassThruSupport.04.04\\openport-j2534"

HRESULT __stdcall DllRegisterServer(void);
HRESULT __stdcall DllUnregisterServer(void);
BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved);

static HINSTANCE g_inst;

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved)
{
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) g_inst = inst;
    return TRUE;
}

static LONG set_sz(HKEY key, const char *name, const char *value)
{
    return RegSetValueExA(key, name, 0, REG_SZ, (const BYTE *)value,
                          (DWORD)strlen(value) + 1);
}

HRESULT __stdcall DllRegisterServer(void)
{
    /* The protocols the cable carries. An application offers only the
     * interfaces whose key has its protocol set to 1. */
    static const char *const protocols[] = { "CAN", "ISO15765", "ISO9141", "ISO14230" };
    char path[MAX_PATH];
    HKEY key;
    LONG rc;
    DWORD n, one = 1;
    size_t i;

    n = GetModuleFileNameA(g_inst, path, sizeof path);
    if (n == 0 || n >= sizeof path) return E_FAIL;

    rc = RegCreateKeyExA(HKEY_LOCAL_MACHINE, OP_REG_KEY, 0, NULL, 0,
                         KEY_SET_VALUE, NULL, &key, NULL);
    if (rc != ERROR_SUCCESS) return HRESULT_FROM_WIN32((DWORD)rc);

    rc = set_sz(key, "Name", "OpenPort 2.0 (openport-j2534)");
    if (rc == ERROR_SUCCESS) rc = set_sz(key, "Vendor", "openport-j2534");
    if (rc == ERROR_SUCCESS) rc = set_sz(key, "FunctionLibrary", path);
    for (i = 0; rc == ERROR_SUCCESS && i < sizeof protocols / sizeof protocols[0]; i++)
        rc = RegSetValueExA(key, protocols[i], 0, REG_DWORD, (const BYTE *)&one, sizeof one);
    RegCloseKey(key);
    return rc == ERROR_SUCCESS ? S_OK : HRESULT_FROM_WIN32((DWORD)rc);
}

HRESULT __stdcall DllUnregisterServer(void)
{
    LONG rc = RegDeleteKeyA(HKEY_LOCAL_MACHINE, OP_REG_KEY);
    if (rc == ERROR_SUCCESS || rc == ERROR_FILE_NOT_FOUND) return S_OK;
    return HRESULT_FROM_WIN32((DWORD)rc);
}
