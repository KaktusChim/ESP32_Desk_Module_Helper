import importlib.util
import os
import sys
from pathlib import Path

RESOLVE_SCRIPT_API = Path(
    r"C:\ProgramData\Blackmagic Design\DaVinci Resolve\Support\Developer\Scripting"
)
RESOLVE_SCRIPT_MODULE = RESOLVE_SCRIPT_API / "Modules" / "DaVinciResolveScript.py"
RESOLVE_INSTALL_DIR = Path(r"C:\Program Files\Blackmagic Design\DaVinci Resolve")
RESOLVE_SCRIPT_LIB = RESOLVE_INSTALL_DIR / "fusionscript.dll"


def load_resolve_module():
    os.environ["RESOLVE_SCRIPT_API"] = str(RESOLVE_SCRIPT_API)
    os.environ["RESOLVE_SCRIPT_LIB"] = str(RESOLVE_SCRIPT_LIB)
    os.environ["PYTHONPATH"] = os.environ.get("PYTHONPATH", "") + ";" + str(RESOLVE_SCRIPT_API / "Modules")

    if RESOLVE_INSTALL_DIR.exists() and hasattr(os, "add_dll_directory"):
        os.add_dll_directory(str(RESOLVE_INSTALL_DIR))

    modules_dir = str(RESOLVE_SCRIPT_API / "Modules")
    if modules_dir not in sys.path:
        sys.path.insert(0, modules_dir)

    import DaVinciResolveScript as bmd
    return bmd


def main():
    if len(sys.argv) < 2:
        raise SystemExit(2)

    target = sys.argv[1]

    try:
        bmd = load_resolve_module()
        resolve = bmd.scriptapp("Resolve")
        if resolve is None:
            raise SystemExit(3)

        ok = resolve.OpenPage(target)
        if ok:
            raise SystemExit(0)
        raise SystemExit(4)

    except Exception:
        raise SystemExit(5)


if __name__ == "__main__":
    main()