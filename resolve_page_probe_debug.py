import importlib.util
from pathlib import Path
import traceback

print("probe start")

possible = [
    Path(r"C:\ProgramData\Blackmagic Design\DaVinci Resolve\Support\Developer\Scripting\Modules\DaVinciResolveScript.py"),
    Path(r"C:\Program Files\Blackmagic Design\DaVinci Resolve\Developer\Scripting\Modules\DaVinciResolveScript.py"),
]

for p in possible:
    print("checking:", p, "exists=", p.exists())

try:
    import DaVinciResolveScript as bmd
    print("imported from PYTHONPATH")
    resolve = bmd.scriptapp("Resolve")
    print("resolve obj:", resolve)
    if resolve:
        print("page:", resolve.GetCurrentPage())
except Exception as e:
    print("direct import failed:")
    traceback.print_exc()

for path in possible:
    if path.exists():
        try:
            print("trying explicit load:", path)
            spec = importlib.util.spec_from_file_location("DaVinciResolveScript", str(path))
            module = importlib.util.module_from_spec(spec)
            spec.loader.exec_module(module)
            resolve = module.scriptapp("Resolve")
            print("resolve obj explicit:", resolve)
            if resolve:
                print("page explicit:", resolve.GetCurrentPage())
        except Exception:
            traceback.print_exc()