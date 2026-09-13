/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Under Wine, a J2534 DLL that expects a USB bulk pipe gets a serial port
 * with zero COMMTIMEOUTS, so an overlapped ReadFile(n) waits for all n bytes.
 * Setting ReadIntervalTimeout=MAXDWORD (only) makes Wine complete an
 * overlapped read as soon as any byte is available and never with zero bytes,
 * which is the bulk-pipe behaviour the DLL was written against. The DLL
 * resolves its imports from the export table at load time, so patching the
 * table entry before LoadLibrary is enough; no instruction patching. */
#include <stdio.h>
#include <string.h>
#include <wchar.h>
#include "wineshim.h"

static FILE *g_log;
static HANDLE (WINAPI *real_CreateFileW)(LPCWSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
static HANDLE (WINAPI *real_CreateFileA)(LPCSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);

static void fix_handle(HANDLE h, const wchar_t *name)
{
    COMMTIMEOUTS ct;
    if (h == INVALID_HANDLE_VALUE || !name || !wcsstr(name, L"vid_0403")) return;
    memset(&ct, 0, sizeof ct);
    ct.ReadIntervalTimeout = MAXDWORD;
    if (g_log) fprintf(g_log, "# wineshim: device handle %p -> COMMTIMEOUTS avail-mode (%s)\n",
                       h, SetCommTimeouts(h, &ct) ? "ok" : "FAILED");
}

static HANDLE WINAPI hook_CreateFileW(LPCWSTR n, DWORD a, DWORD s, LPSECURITY_ATTRIBUTES sa, DWORD d, DWORD f, HANDLE t)
{
    HANDLE h = real_CreateFileW(n, a, s, sa, d, f, t);
    fix_handle(h, n);
    return h;
}

static HANDLE WINAPI hook_CreateFileA(LPCSTR n, DWORD a, DWORD s, LPSECURITY_ATTRIBUTES sa, DWORD d, DWORD f, HANDLE t)
{
    wchar_t w[1024];
    HANDLE h = real_CreateFileA(n, a, s, sa, d, f, t);
    if (n && MultiByteToWideChar(CP_ACP, 0, n, -1, w, 1024)) fix_handle(h, w);
    return h;
}

static void *eat_patch(HMODULE mod, const char *name, void *newfn)
{
    IMAGE_DOS_HEADER *dos = (IMAGE_DOS_HEADER *)mod;
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)((char *)mod + dos->e_lfanew);
    IMAGE_DATA_DIRECTORY *dd = &nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
    IMAGE_EXPORT_DIRECTORY *ed = (IMAGE_EXPORT_DIRECTORY *)((char *)mod + dd->VirtualAddress);
    DWORD *names = (DWORD *)((char *)mod + ed->AddressOfNames);
    WORD *ords = (WORD *)((char *)mod + ed->AddressOfNameOrdinals);
    DWORD *funcs = (DWORD *)((char *)mod + ed->AddressOfFunctions);
    DWORD i;
    for (i = 0; i < ed->NumberOfNames; i++) {
        if (strcmp((char *)mod + names[i], name) != 0) continue;
        DWORD *slot = &funcs[ords[i]], old;
        void *orig = (char *)mod + *slot;
        if ((char *)orig >= (char *)ed && (char *)orig < (char *)ed + dd->Size)
            orig = (void *)GetProcAddress(mod, name);   /* forwarder string */
        if (!VirtualProtect(slot, sizeof *slot, PAGE_READWRITE, &old)) return NULL;
        *slot = (DWORD)((char *)newfn - (char *)mod);
        VirtualProtect(slot, sizeof *slot, old, &old);
        return orig;
    }
    return NULL;
}

int wineshim_active(void)
{
    HMODULE nt = GetModuleHandleA("ntdll.dll");
    return nt && GetProcAddress(nt, "wine_get_version") != NULL;
}

void wineshim_install(FILE *log)
{
    HMODULE k32 = GetModuleHandleA("kernel32.dll");
    g_log = log;
    real_CreateFileW = eat_patch(k32, "CreateFileW", (void *)hook_CreateFileW);
    real_CreateFileA = eat_patch(k32, "CreateFileA", (void *)hook_CreateFileA);
    if (g_log) fprintf(g_log, "# wineshim: CreateFileW=%p CreateFileA=%p patched in kernel32\n",
                       (void *)real_CreateFileW, (void *)real_CreateFileA);
}
