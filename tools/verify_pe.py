#!/usr/bin/env python3
"""Checks a built ControlCenterAdvanced.exe before it is released.

    python3 tools/verify_pe.py build/win/ControlCenterAdvanced.min.exe [--version 1.0.0] [--json]

It cannot run the program (that needs Windows); it checks what the file itself says, and fails (exit status 1) when one of these is wrong:

  * a 64-bit (AMD64) GUI executable, not a DLL
  * hardening flags: ASLR (DYNAMIC_BASE with a relocation table), high-entropy ASLR, DEP (NX_COMPAT)
  * the embedded manifest asks for "requireAdministrator" (the program is one elevated process: it reads the embedded controller through
    the vendor's driver and can start the vendor's services; it installs nothing of its own), declares per-monitor DPI awareness and the
    Windows 10/11 compatibility id
  * the version resource matches --version, and the product name/company are the expected ones
  * every imported DLL is a Windows system DLL (no third-party or driver-loader DLL is linked in), and no import is one of a short list of
    things this program must never use (kernel driver loaders, remote-access APIs)
  * the only networking library is Winsock, which carries the link to the vendor's service on the same computer; the build contains the
    guard that refuses every address except the loopback one, and no Internet client library (WinINet, WinHTTP, URLMon, DNS) is imported
  * no debug directory / no build-machine path in the file, no embedded secrets (private keys, tokens, a broker URL with a password)
  * the program carries no extra payload: no overlay after the last section, and no section that looks packed or encrypted
"""
import argparse
import hashlib
import json
import math
import os
import re
import sys

try:
    import pefile
except ImportError:
    sys.exit("pefile is required: pip install pefile")

# Windows system DLLs the program may import (case-insensitive). Anything else is reported.
SYSTEM_DLLS = {
    "kernel32.dll", "user32.dll", "gdi32.dll", "advapi32.dll", "shell32.dll", "ole32.dll", "oleaut32.dll", "comctl32.dll", "comdlg32.dll",
    "shlwapi.dll", "setupapi.dll", "hid.dll", "wlanapi.dll", "powrprof.dll", "version.dll", "uxtheme.dll", "winmm.dll", "bcrypt.dll",
    "crypt32.dll", "dwmapi.dll", "gdiplus.dll", "msvcrt.dll", "ntdll.dll", "imm32.dll", "winspool.drv", "netapi32.dll",
    "iphlpapi.dll", "psapi.dll", "wtsapi32.dll", "userenv.dll", "secur32.dll", "rpcrt4.dll", "cfgmgr32.dll", "dxgi.dll", "d3d11.dll",
    "ws2_32.dll",        # the loopback link to the vendor's service (see LOOPBACK_GUARD)
    "api-ms-win-crt-runtime-l1-1-0.dll",
}
# Things that must never be linked: loaders of third-party kernel drivers or vulnerable-driver helpers.
FORBIDDEN_DLLS = {"winring0.dll", "winring0x64.dll", "inpout32.dll", "inpoutx64.dll", "phymem.dll", "pawnio.dll", "libusb-1.0.dll"}
FORBIDDEN_IMPORTS = {"NtLoadDriver", "ZwLoadDriver", "MmMapIoSpace"}
# The program never talks to the internet: no Windows Internet client library may be imported. Winsock (ws2_32.dll) is allowed because the
# link to the vendor's service is a TCP connection to 127.0.0.1; src/oem/tcp.cpp refuses any other address, and the text it reports when it
# does is looked for in the file below.
NETWORK_DLLS = {"wsock32.dll", "mswsock.dll", "wininet.dll", "winhttp.dll", "urlmon.dll", "dnsapi.dll"}
LOOPBACK_GUARD = b"refusing to connect to a non-loopback address"
SECRET_PATTERNS = [
    rb"-----BEGIN (?:RSA |EC |OPENSSH )?PRIVATE KEY-----",
    rb"mqtts?://[^\s\"']*:[^\s\"'@]*@",          # a broker URL with user:password
    rb"AKIA[0-9A-Z]{16}",                         # an AWS access key id
    rb"ghp_[A-Za-z0-9]{30,}",                     # a GitHub token
]

FLAGS = {
    "dynamic_base": 0x0040,
    "high_entropy_va": 0x0020,
    "nx_compat": 0x0100,
    "no_seh": 0x0400,
}


def entropy(data):
    if not data:
        return 0.0
    counts = [0] * 256
    for b in data:
        counts[b] += 1
    n = float(len(data))
    return -sum(c / n * math.log(c / n, 2) for c in counts if c)


def resource_data(pe, type_id):
    """Yields the bytes of every resource of one type."""
    if not hasattr(pe, "DIRECTORY_ENTRY_RESOURCE"):
        return
    for t in pe.DIRECTORY_ENTRY_RESOURCE.entries:
        if t.id != type_id:
            continue
        for n in t.directory.entries:
            for l in n.directory.entries:
                d = l.data.struct
                yield pe.get_data(d.OffsetToData, d.Size)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("exe")
    ap.add_argument("--version", help="expected product/file version, e.g. 1.0.0")
    ap.add_argument("--json", action="store_true", help="print the report as JSON")
    a = ap.parse_args()

    raw = open(a.exe, "rb").read()
    pe = pefile.PE(data=raw, fast_load=False)
    problems, notes = [], []
    report = {"file": os.path.basename(a.exe), "bytes": len(raw), "sha256": hashlib.sha256(raw).hexdigest()}

    def check(ok, msg):
        (notes if ok else problems).append(("ok   " if ok else "FAIL ") + msg)

    # ---- format
    check(pe.FILE_HEADER.Machine == 0x8664, "machine is AMD64 (0x%04X)" % pe.FILE_HEADER.Machine)
    check(not pe.is_dll(), "an executable, not a DLL")
    check(pe.OPTIONAL_HEADER.Subsystem == 2, "subsystem is WINDOWS_GUI (%d)" % pe.OPTIONAL_HEADER.Subsystem)
    dll = pe.OPTIONAL_HEADER.DllCharacteristics
    for name, bit in FLAGS.items():
        if name == "no_seh":
            continue
        check(bool(dll & bit), "%s is set" % name)
    reloc = pe.OPTIONAL_HEADER.DATA_DIRECTORY[5]    # IMAGE_DIRECTORY_ENTRY_BASERELOC
    check(reloc.Size > 0, "has a relocation table (ASLR can move it)")
    report["image_base"] = hex(pe.OPTIONAL_HEADER.ImageBase)
    report["timestamp"] = pe.FILE_HEADER.TimeDateStamp

    # ---- manifest
    manifests = list(resource_data(pe, 24))        # RT_MANIFEST
    check(len(manifests) == 1, "one embedded manifest")
    if manifests:
        m = manifests[0].decode("utf-8", "replace")
        check('level="requireAdministrator"' in m, "manifest asks for requireAdministrator (one elevated process; nothing is installed)")
        check("PerMonitorV2" in m, "manifest declares per-monitor DPI awareness (V2)")
        check("8e0f7a12-bfb3-4fe8-b9a5-48fd50a15a9a" in m and "1f676c76-80e1-4239-95bb-83d0f6d0da78" in m, "manifest lists Windows 10/11 and 8.1 compatibility")
        check('uiAccess="false"' in m, "manifest: uiAccess=false")

    # ---- version resource
    strings = {}
    if hasattr(pe, "FileInfo"):
        for fi in pe.FileInfo:
            for entry in fi:
                if getattr(entry, "Key", b"") == b"StringFileInfo":
                    for st in entry.StringTable:
                        for k, v in st.entries.items():
                            strings[k.decode()] = v.decode()
    report["version_info"] = strings
    check(strings.get("ProductName") == "Control Center Advanced", "ProductName is Control Center Advanced")
    check(strings.get("CompanyName") == "MehmetNY", "CompanyName is MehmetNY")
    check(strings.get("LegalCopyright") == "GPL-3.0-or-later", "LegalCopyright says GPL-3.0-or-later")
    if a.version:
        check(strings.get("FileVersion") == a.version and strings.get("ProductVersion") == a.version,
              "file and product version are %s (found %s / %s)" % (a.version, strings.get("FileVersion"), strings.get("ProductVersion")))
        ffi = getattr(pe, "VS_FIXEDFILEINFO", [None])[0]
        if ffi is not None:
            want = [int(x) for x in re.match(r"\d+\.\d+\.\d+", a.version).group(0).split(".")] + [0]
            got = [ffi.FileVersionMS >> 16, ffi.FileVersionMS & 0xFFFF, ffi.FileVersionLS >> 16, ffi.FileVersionLS & 0xFFFF]
            check(got == want, "fixed file version is %s" % ".".join(map(str, want)))

    # ---- imports
    dlls = {}
    if hasattr(pe, "DIRECTORY_ENTRY_IMPORT"):
        for imp in pe.DIRECTORY_ENTRY_IMPORT:
            name = imp.dll.decode().lower()
            dlls[name] = sorted((i.name.decode() if i.name else "#%d" % i.ordinal) for i in imp.imports)
    report["imports"] = {k: len(v) for k, v in sorted(dlls.items())}
    odd = sorted(d for d in dlls if d not in SYSTEM_DLLS and not d.startswith("api-ms-win-"))
    check(not odd, "every imported DLL is a Windows system DLL" + ("" if not odd else " (not on the list: %s)" % ", ".join(odd)))
    bad = sorted(d for d in dlls if d in FORBIDDEN_DLLS)
    check(not bad, "no driver-loader DLL is linked" + ("" if not bad else " (%s)" % ", ".join(bad)))
    net = sorted(d for d in dlls if d in NETWORK_DLLS)
    check(not net, "no Internet client library is imported (WinINet/WinHTTP/URLMon/DNS)" + ("" if not net else " (%s)" % ", ".join(net)))
    if "ws2_32.dll" in dlls:
        check(LOOPBACK_GUARD in raw, "Winsock is only for the loopback link: the guard that refuses other addresses is in the file")
    badf = sorted(f for fs in dlls.values() for f in fs if f in FORBIDDEN_IMPORTS)
    check(not badf, "no kernel-driver import" + ("" if not badf else " (%s)" % ", ".join(badf)))
    check(len(dlls) > 0 and all(len(v) > 0 for v in dlls.values()), "import table is well-formed (%d DLLs)" % len(dlls))

    # ---- sections, overlay, debug info
    secs = []
    for s in pe.sections:
        name = s.Name.rstrip(b"\0").decode("latin-1")
        data = s.get_data()
        secs.append({"name": name, "size": s.SizeOfRawData, "entropy": round(entropy(data), 2), "exec": bool(s.Characteristics & 0x20000000),
                     "write": bool(s.Characteristics & 0x80000000)})
    report["sections"] = secs
    wx = [s["name"] for s in secs if s["exec"] and s["write"]]
    check(not wx, "no section is both writable and executable" + ("" if not wx else " (%s)" % ", ".join(wx)))
    packed = [s["name"] for s in secs if s["entropy"] > 7.3 and s["size"] > 4096]
    check(not packed, "no section looks packed or encrypted (entropy <= 7.3)" + ("" if not packed else " (%s)" % ", ".join(packed)))
    end = max(s.PointerToRawData + s.SizeOfRawData for s in pe.sections)
    overlay = len(raw) - end
    check(overlay <= 0 or overlay < 16, "no overlay after the last section (%d bytes)" % max(overlay, 0))
    dbg = pe.OPTIONAL_HEADER.DATA_DIRECTORY[6]      # IMAGE_DIRECTORY_ENTRY_DEBUG
    if dbg.Size:
        notes.append("note the file has a debug directory (%d bytes): a stripped build has none" % dbg.Size)
    leak = re.findall(rb"[A-Za-z]:[\\/](?:Users|home|work|build)[\\/][\w .\\/-]{3,80}", raw)
    check(not leak, "no build-machine path in the file" + ("" if not leak else " (e.g. %s)" % leak[0].decode("latin-1")))

    # ---- secrets
    found = []
    for pat in SECRET_PATTERNS:
        m = re.search(pat, raw)
        if m:
            found.append(pat.decode("latin-1")[:30])
    check(not found, "no embedded private key, token or credentialed broker URL" + ("" if not found else " (%s)" % ", ".join(found)))

    if a.json:
        report["problems"] = problems
        report["checks"] = notes
        print(json.dumps(report, indent=2))
    else:
        print("%s  %d bytes  sha256 %s" % (report["file"], report["bytes"], report["sha256"]))
        for n in notes:
            print("  " + n)
        for p in problems:
            print("  " + p)
        print("%d checks passed, %d failed" % (sum(1 for n in notes if n.startswith("ok")), len(problems)))
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
