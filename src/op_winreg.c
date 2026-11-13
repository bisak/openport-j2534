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
 * The values are the ones Tactrix's installer writes for op20pt32.dll, so an
 * application that picks interfaces by protocol offers this one wherever it
 * offers Tactrix's. The wide-character calls keep a DLL path outside the
 * system code page intact.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <windows.h>

#include <wchar.h>

#define OP_REG_PARENT L"SOFTWARE\\PassThruSupport.04.04"
#define OP_REG_KEY    OP_REG_PARENT L"\\openport-j2534"

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

static LONG set_sz(HKEY key, const wchar_t *name, const wchar_t *value)
{
    return RegSetValueExW(key, name, 0, REG_SZ, (const BYTE *)value,
                          (DWORD)((wcslen(value) + 1) * sizeof(wchar_t)));
}

static LONG set_dword(HKEY key, const wchar_t *name, DWORD value)
{
    return RegSetValueExW(key, name, 0, REG_DWORD, (const BYTE *)&value, sizeof value);
}

HRESULT __stdcall DllRegisterServer(void)
{
    static const struct { const wchar_t *name; DWORD on; } protocols[] = {
        { L"CAN", 1 }, { L"ISO15765", 1 },
        { L"ISO9141", 1 }, { L"ISO9141_CH1", 1 }, { L"ISO9141_CH2", 1 }, { L"ISO9141_CH3", 1 },
        { L"ISO14230", 1 }, { L"ISO14230_CH1", 1 }, { L"ISO14230_CH2", 1 },
        { L"J1850PWM", 0 }, { L"J1850VPW", 0 },
    };
    wchar_t path[32768];
    HKEY key;
    LONG rc;
    DWORD n;
    size_t i;

    n = GetModuleFileNameW(g_inst, path, (DWORD)(sizeof path / sizeof path[0]));
    if (n == 0 || n >= sizeof path / sizeof path[0]) return E_FAIL;

    rc = RegCreateKeyExW(HKEY_LOCAL_MACHINE, OP_REG_KEY, 0, NULL, 0,
                         KEY_SET_VALUE, NULL, &key, NULL);
    if (rc != ERROR_SUCCESS) return HRESULT_FROM_WIN32((DWORD)rc);

    rc = set_sz(key, L"Name", L"OpenPort 2.0 (openport-j2534)");
    if (rc == ERROR_SUCCESS) rc = set_sz(key, L"Vendor", L"openport-j2534");
    if (rc == ERROR_SUCCESS) rc = set_sz(key, L"ConfigApplication", L"");
    if (rc == ERROR_SUCCESS) rc = set_sz(key, L"FunctionLibrary", path);
    if (rc == ERROR_SUCCESS) rc = set_dword(key, L"DeviceId", 0);
    for (i = 0; rc == ERROR_SUCCESS && i < sizeof protocols / sizeof protocols[0]; i++)
        rc = set_dword(key, protocols[i].name, protocols[i].on);
    RegCloseKey(key);
    return rc == ERROR_SUCCESS ? S_OK : HRESULT_FROM_WIN32((DWORD)rc);
}

/* Removes this DLL's key, and the parent too when nothing else is left in
 * it: on a 64-bit machine the 64-bit view usually had no parent before. */
HRESULT __stdcall DllUnregisterServer(void)
{
    LONG rc = RegDeleteKeyW(HKEY_LOCAL_MACHINE, OP_REG_KEY);
    HKEY parent;

    if (rc != ERROR_SUCCESS && rc != ERROR_FILE_NOT_FOUND)
        return HRESULT_FROM_WIN32((DWORD)rc);
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, OP_REG_PARENT, 0, KEY_READ, &parent) == ERROR_SUCCESS) {
        DWORD subkeys = 1, values = 1;
        LONG q = RegQueryInfoKeyW(parent, NULL, NULL, NULL, &subkeys, NULL, NULL,
                                  &values, NULL, NULL, NULL, NULL);
        RegCloseKey(parent);
        if (q == ERROR_SUCCESS && subkeys == 0 && values == 0)
            (void)RegDeleteKeyW(HKEY_LOCAL_MACHINE, OP_REG_PARENT);
    }
    return S_OK;
}
