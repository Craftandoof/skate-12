#!/usr/bin/env python3
"""Make the upstream Skate 3 Mobile APK installable on Android 12 (API 31).

No game files are needed: the upstream release already contains the compiled
native code. This script only changes the Java side of that public APK:

  1. AndroidManifest.xml: minSdkVersion 33 -> 31.
  2. HIDDeviceManager: the two Context.registerReceiver(..., int flags) calls
     only exist on API 33+. On Android 12 they throw NoSuchMethodError as the
     game starts. They are replaced by a helper that calls the old two-argument
     form below API 33 and the original call on API 33+, so Android 13+ behaves
     exactly as before.

Usage: patch_upstream_apk.py APKTOOL_JAR INPUT_APK OUTPUT_UNSIGNED_APK
The output still has to be zip-aligned and signed (see the workflow).
"""
import re
import shutil
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

OLD_MIN_SDK = 33
NEW_MIN_SDK = 31

INVOKE = re.compile(
    r"invoke-virtual \{(v\d+), (v\d+), (v\d+), (v\d+)\}, "
    r"Landroid/content/Context;->registerReceiver\("
    r"Landroid/content/BroadcastReceiver;Landroid/content/IntentFilter;I\)"
    r"Landroid/content/Intent;")

HELPER = """
.method private static registerReceiverExported(Landroid/content/Context;Landroid/content/BroadcastReceiver;Landroid/content/IntentFilter;)V
    .locals 2

    sget v0, Landroid/os/Build$VERSION;->SDK_INT:I

    const/16 v1, 0x21

    if-lt v0, v1, :cond_legacy

    const/4 v0, 0x2

    invoke-virtual {p0, p1, p2, v0}, Landroid/content/Context;->registerReceiver(Landroid/content/BroadcastReceiver;Landroid/content/IntentFilter;I)Landroid/content/Intent;

    return-void

    :cond_legacy
    invoke-virtual {p0, p1, p2}, Landroid/content/Context;->registerReceiver(Landroid/content/BroadcastReceiver;Landroid/content/IntentFilter;)Landroid/content/Intent;

    return-void
.end method
"""


def run(*args):
    subprocess.run([str(a) for a in args], check=True)


def patch_smali(root: Path) -> None:
    files = list(root.glob("smali*/org/libsdl/app/HIDDeviceManager.smali"))
    if len(files) != 1:
        sys.exit(f"expected one HIDDeviceManager.smali, found {len(files)}")
    path = files[0]
    text = path.read_text()

    def replace(match):
        context, receiver, filt, flags = match.groups()
        return (f"invoke-static {{{context}, {receiver}, {filt}}}, "
                "Lorg/libsdl/app/HIDDeviceManager;->registerReceiverExported("
                "Landroid/content/Context;Landroid/content/BroadcastReceiver;"
                "Landroid/content/IntentFilter;)V")

    text, count = INVOKE.subn(replace, text)
    if count != 2:
        sys.exit(f"expected 2 registerReceiver calls to patch, found {count}. "
                 "The upstream APK changed; update this script.")
    if ".method private static registerReceiverExported" not in text:
        text = text.rstrip("\n") + "\n" + HELPER
    path.write_text(text)
    print(f"patched {count} registerReceiver calls in {path.name}")


def patch_manifest(manifest: Path) -> None:
    """Change the minSdkVersion integer inside the binary manifest.

    The value is stored as a typed integer: size 8, res0 0, type 0x10, data.
    Exactly one such entry must hold the old value (targetSdkVersion differs).
    """
    data = bytearray(manifest.read_bytes())
    needle = struct.pack("<HBBI", 8, 0, 0x10, OLD_MIN_SDK)
    hits = [m.start() for m in re.finditer(re.escape(needle), bytes(data))]
    if len(hits) != 1:
        sys.exit(f"expected one minSdkVersion={OLD_MIN_SDK} entry, found {len(hits)}")
    data[hits[0] + 4:hits[0] + 8] = struct.pack("<I", NEW_MIN_SDK)
    manifest.write_bytes(bytes(data))
    print(f"minSdkVersion {OLD_MIN_SDK} -> {NEW_MIN_SDK}")


def main() -> None:
    if len(sys.argv) != 4:
        sys.exit(__doc__)
    apktool, source, output = (Path(a).resolve() for a in sys.argv[1:])
    work = Path(tempfile.mkdtemp(prefix="skate3-repack-"))
    try:
        decoded = work / "decoded"
        # -r keeps resources.arsc and the manifest untouched; only code is edited.
        run("java", "-jar", apktool, "d", "-r", "-f", "-o", decoded, source)
        patch_smali(decoded)
        patch_manifest(decoded / "AndroidManifest.xml")
        output.parent.mkdir(parents=True, exist_ok=True)
        run("java", "-jar", apktool, "b", "-f", "-o", output, decoded)
    finally:
        shutil.rmtree(work, ignore_errors=True)


if __name__ == "__main__":
    main()
