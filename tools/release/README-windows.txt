openport-j2534 for Windows
==========================

A J2534 PassThru driver for the Tactrix OpenPort 2.0 cable.
https://github.com/bisak/openport-j2534

  win32\openport-j2534-32.dll   for 32-bit J2534 applications (most of them)
  win64\openport-j2534-64.dll   for 64-bit J2534 applications
  win32\op_probe.exe, win64\op_probe.exe
                                which USB driver the cable has

Install
-------
Put the folder where it will stay, then register each DLL from an
administrator Command Prompt in that folder:

  C:\Windows\SysWOW64\regsvr32 win32\openport-j2534-32.dll
  C:\Windows\System32\regsvr32 win64\openport-j2534-64.dll

J2534 applications then list "OpenPort 2.0 (openport-j2534)", beside
Tactrix's own entry if that is installed. Neither DLL needs another file.

Remove
------
  C:\Windows\SysWOW64\regsvr32 /u win32\openport-j2534-32.dll
  C:\Windows\System32\regsvr32 /u win64\openport-j2534-64.dll

The cable's USB driver
----------------------
Tactrix's driver (installed with Tactrix's software), or, with no driver
installed, the USB serial driver Windows 10 and later give the cable by
themselves. op_probe.exe shows which one the cable has. Tactrix's driver is
the one tested with this release.

Problems
--------
Set OPENPORT_LOG=C:\path\to\openport.log before starting the application,
reproduce the problem, and attach the log to an issue on GitHub. Add
OPENPORT_LOG_HEX=1 to log every byte sent and received.
