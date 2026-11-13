#!/usr/bin/env python3
"""
registry_check — the DLL installed and found the way a J2534 application
finds it, on Windows.

  python tests/windows/registry_check.py build/win64/openport-j2534-64.dll

Run it from an administrator prompt, with a Python of the DLL's width (a
32-bit Python for the 32-bit DLL). It registers the DLL with regsvr32, reads
the key back from the registry view an application of that width reads, loads
the DLL from the key's FunctionLibrary path, opens the cable (or, with none
attached, expects ERR_DEVICE_NOT_CONNECTED), unregisters it, and checks the
registry is as it was. Exit status 0 means every check passed.

SPDX-License-Identifier: GPL-3.0-or-later
"""
import ctypes
import os
import struct
import subprocess
import sys
import winreg

PARENT = r"SOFTWARE\PassThruSupport.04.04"
KEY = PARENT + r"\openport-j2534"
EXPECTED = {
    "Name": "OpenPort 2.0 (openport-j2534)", "Vendor": "openport-j2534",
    "ConfigApplication": "", "DeviceId": 0,
    "CAN": 1, "ISO15765": 1, "ISO9141": 1, "ISO9141_CH1": 1, "ISO9141_CH2": 1,
    "ISO9141_CH3": 1, "ISO14230": 1, "ISO14230_CH1": 1, "ISO14230_CH2": 1,
    "J1850PWM": 0, "J1850VPW": 0,
}
ERR_DEVICE_NOT_CONNECTED = 8

failures = 0


def check(ok, what, detail=""):
    global failures
    print(f"    {'ok  ' if ok else 'FAIL'}  {what}" + (f" ({detail})" if detail and not ok else ""))
    if not ok:
        failures += 1


def view_flag(bits):
    return winreg.KEY_WOW64_32KEY if bits == 32 else winreg.KEY_WOW64_64KEY


def read_key(bits):
    try:
        with winreg.OpenKey(winreg.HKEY_LOCAL_MACHINE, KEY, 0,
                            winreg.KEY_READ | view_flag(bits)) as k:
            out, i = {}, 0
            while True:
                try:
                    name, value, _ = winreg.EnumValue(k, i)
                except OSError:
                    return out
                out[name] = value
                i += 1
    except FileNotFoundError:
        return None


def exists(path, bits):
    try:
        winreg.CloseKey(winreg.OpenKey(winreg.HKEY_LOCAL_MACHINE, path, 0,
                                       winreg.KEY_READ | view_flag(bits)))
        return True
    except FileNotFoundError:
        return False


def regsvr32(dll, unregister=False):
    # From a process of the DLL's width, System32 is the regsvr32 of that
    # width: a 32-bit process is redirected to SysWOW64.
    exe = os.path.join(os.environ.get("SystemRoot", r"C:\Windows"), "System32", "regsvr32.exe")
    return subprocess.run([exe, "/s"] + (["/u"] if unregister else []) + [dll]).returncode


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    dll = os.path.abspath(sys.argv[1])
    bits = struct.calcsize("P") * 8
    print(f"DLL {dll}, a {bits}-bit Python")
    parent_before = exists(PARENT, bits)
    if read_key(bits) is not None:
        sys.exit("an openport-j2534 key is already registered; unregister it first")

    check(regsvr32(dll) == 0, "regsvr32 registers the DLL")
    values = read_key(bits)
    check(values is not None, f"the key is in the {bits}-bit view")
    values = values or {}
    for name, want in EXPECTED.items():
        check(values.get(name) == want, f"{name} = {want!r}", repr(values.get(name)))
    check(os.path.normcase(values.get("FunctionLibrary", "")) == os.path.normcase(dll),
          "FunctionLibrary is the DLL's own path", values.get("FunctionLibrary"))

    lib = ctypes.WinDLL(values.get("FunctionLibrary", dll))
    for f in ("PassThruOpen", "PassThruClose", "PassThruConnect", "PassThruDisconnect",
              "PassThruReadMsgs", "PassThruWriteMsgs", "PassThruStartPeriodicMsg",
              "PassThruStopPeriodicMsg", "PassThruStartMsgFilter", "PassThruStopMsgFilter",
              "PassThruSetProgrammingVoltage", "PassThruReadVersion",
              "PassThruGetLastError", "PassThruIoctl"):
        check(hasattr(lib, f), f"{f} is exported under its plain name")
    dev = ctypes.c_ulong()
    rc = lib.PassThruOpen(None, ctypes.byref(dev))
    text = ctypes.create_string_buffer(80)
    lib.PassThruGetLastError(text)
    if rc == 0:
        fw, dllv, api = (ctypes.create_string_buffer(80) for _ in range(3))
        check(lib.PassThruReadVersion(dev, fw, dllv, api) == 0, "a cable is attached: ReadVersion",
              text.value.decode(errors="replace"))
        print(f"          firmware {fw.value.decode()}, DLL {dllv.value.decode()}, API {api.value.decode()}")
        check(lib.PassThruClose(dev) == 0, "and Close")
    else:
        check(rc == ERR_DEVICE_NOT_CONNECTED, "no cable: Open returns ERR_DEVICE_NOT_CONNECTED",
              f"{rc}: {text.value.decode(errors='replace')}")
    del lib

    check(regsvr32(dll, unregister=True) == 0, "regsvr32 /u unregisters the DLL")
    check(read_key(bits) is None, "the key is gone")
    check(exists(PARENT, bits) == parent_before,
          "PassThruSupport.04.04 is as it was" + ("" if parent_before else " (absent)"))
    print(f"\n{'all checks passed' if not failures else f'{failures} check(s) failed'}")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
