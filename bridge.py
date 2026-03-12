import asyncio
import ctypes
import json
import os
import socket
import subprocess
import sys
import time
from ctypes import wintypes
from pathlib import Path

import keyboard

HOST = "0.0.0.0"
PORT = 8765
POLL_INTERVAL = 0.35
PROFILE_PATH = "resolve_profile.json"

PAGE_PROBE_SCRIPT = "resolve_page_probe.py"
PAGE_PROBE_INTERVAL = 0.7

# -----------------------------
# Win32 foreground window utils
# -----------------------------

user32 = ctypes.WinDLL("user32", use_last_error=True)
kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)

GetForegroundWindow = user32.GetForegroundWindow
GetForegroundWindow.restype = wintypes.HWND

GetWindowTextLengthW = user32.GetWindowTextLengthW
GetWindowTextLengthW.argtypes = [wintypes.HWND]
GetWindowTextLengthW.restype = ctypes.c_int

GetWindowTextW = user32.GetWindowTextW
GetWindowTextW.argtypes = [wintypes.HWND, wintypes.LPWSTR, ctypes.c_int]
GetWindowTextW.restype = ctypes.c_int

GetWindowThreadProcessId = user32.GetWindowThreadProcessId
GetWindowThreadProcessId.argtypes = [wintypes.HWND, ctypes.POINTER(wintypes.DWORD)]
GetWindowThreadProcessId.restype = wintypes.DWORD

OpenProcess = kernel32.OpenProcess
OpenProcess.argtypes = [wintypes.DWORD, wintypes.BOOL, wintypes.DWORD]
OpenProcess.restype = wintypes.HANDLE

CloseHandle = kernel32.CloseHandle
CloseHandle.argtypes = [wintypes.HANDLE]
CloseHandle.restype = wintypes.BOOL

QueryFullProcessImageNameW = kernel32.QueryFullProcessImageNameW
QueryFullProcessImageNameW.argtypes = [
    wintypes.HANDLE,
    wintypes.DWORD,
    wintypes.LPWSTR,
    ctypes.POINTER(wintypes.DWORD),
]
QueryFullProcessImageNameW.restype = wintypes.BOOL

PROCESS_QUERY_LIMITED_INFORMATION = 0x1000

# -----------------------------
# App state
# -----------------------------

clients = set()
last_active_state = None
profile_mtime = None
profile_data = None
current_page_id = None
last_resolve_page = None

# -----------------------------
# Helpers
# -----------------------------


def get_foreground_window_info():
    hwnd = GetForegroundWindow()
    if not hwnd:
        return {"title": "", "process_name": "", "hwnd": None}

    length = GetWindowTextLengthW(hwnd)
    buf = ctypes.create_unicode_buffer(length + 1)
    GetWindowTextW(hwnd, buf, length + 1)
    title = buf.value

    pid = wintypes.DWORD()
    GetWindowThreadProcessId(hwnd, ctypes.byref(pid))

    process_name = ""
    hproc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, False, pid.value)
    if hproc:
        try:
            size = wintypes.DWORD(1024)
            pbuf = ctypes.create_unicode_buffer(size.value)
            if QueryFullProcessImageNameW(hproc, 0, pbuf, ctypes.byref(size)):
                full_path = pbuf.value
                process_name = full_path.split("\\")[-1]
        finally:
            CloseHandle(hproc)

    return {
        "title": title,
        "process_name": process_name,
        "hwnd": hwnd,
    }


def is_resolve_active():
    info = get_foreground_window_info()
    title = (info["title"] or "").lower()
    proc = (info["process_name"] or "").lower()

    return (
        "davinci resolve" in title
        or proc == "resolve.exe"
        or "resolve" in title
    )


def load_profile():
    global profile_data, profile_mtime, current_page_id

    st = os.stat(PROFILE_PATH)
    if profile_mtime == st.st_mtime and profile_data is not None:
        return False

    with open(PROFILE_PATH, "r", encoding="utf-8") as f:
        data = json.load(f)

    if "pages" not in data or not data["pages"]:
        raise ValueError("Profile must contain at least one page in 'pages'.")

    page_ids = [p["page_id"] for p in data["pages"]]
    default_page = data.get("default_page", page_ids[0])

    if current_page_id not in page_ids:
        current_page_id = default_page

    profile_data = data
    profile_mtime = st.st_mtime
    print(f"[PROFILE] Loaded '{PROFILE_PATH}'")
    return True


def get_page(page_id):
    for page in profile_data["pages"]:
        if page["page_id"] == page_id:
            return page
    return None


def build_layout(active: bool):
    global current_page_id

    if not profile_data:
        raise RuntimeError("Profile not loaded")

    if not active:
        return {
            "type": "set_layout",
            "profile": "inactive",
            "page_id": "inactive",
            "title": "Resolve není aktivní",
            "active": False,
            "buttons": [
                {"slot": 0, "label": "—", "action": "noop"},
                {"slot": 1, "label": "—", "action": "noop"},
                {"slot": 2, "label": "—", "action": "noop"},
                {"slot": 3, "label": "—", "action": "noop"},
                {"slot": 4, "label": "—", "action": "noop"},
                {"slot": 5, "label": "—", "action": "noop"},
            ],
        }

    page = get_page(current_page_id)
    if page is None:
        current_page_id = profile_data.get("default_page", profile_data["pages"][0]["page_id"])
        page = get_page(current_page_id)

    return {
        "type": "set_layout",
        "profile": profile_data.get("profile_id", "resolve"),
        "page_id": page["page_id"],
        "title": page.get("title", "Resolve"),
        "active": True,
        "buttons": page["buttons"],
        "pages": [
            {
                "page_id": p["page_id"],
                "title": p.get("title", p["page_id"])
            }
            for p in profile_data["pages"]
        ],
    }


def get_action_definition(action_id: str):
    for action in profile_data.get("actions", []):
        if action["action_id"] == action_id:
            return action
    return None


def execute_action(action_id: str):
    action = get_action_definition(action_id)
    if action is None:
        print(f"[WARN] Unknown action: {action_id}")
        return False, f"Unknown action: {action_id}"

    action_type = action.get("type", "shortcut")

    if action_type == "shortcut":
        keys = action.get("keys", [])
        if not keys:
            return False, f"{action_id} has no keys"

        combo = "+".join(keys)
        keyboard.send(combo)
        return True, f"{action_id} -> {combo}"

    if action_type == "macro":
        for step in action.get("steps", []):
            step_type = step.get("type")

            if step_type == "shortcut":
                combo = "+".join(step.get("keys", []))
                keyboard.send(combo)

            elif step_type == "delay_ms":
                time.sleep(step.get("value", 0) / 1000.0)

            else:
                raise ValueError(f"Unsupported macro step: {step_type}")

        return True, f"{action_id} macro done"

    raise ValueError(f"Unsupported action type: {action_type}")

def open_resolve_page(target_page: str) -> bool:
    page_map = {
        "edit": "edit",
        "color": "color",
        "audio": "fairlight",
        "deliver": "deliver",
    }

    resolve_page = page_map.get(target_page)
    if resolve_page is None:
        return False

    helper = Path(__file__).with_name("resolve_open_page.py")
    if not helper.exists():
        return False

    try:
        result = subprocess.run(
            [sys.executable, str(helper), resolve_page],
            capture_output=True,
            text=True,
            timeout=2.0,
            creationflags=subprocess.CREATE_NO_WINDOW if os.name == "nt" else 0,
        )
        return result.returncode == 0
    except Exception as e:
        print(f"[OPEN PAGE] failed: {e}")
        return False

def probe_resolve_page():
    """
    Spustí helper v odděleném procesu.
    Když helper spadne, hlavní bridge zůstane živý.
    """
    helper = Path(__file__).with_name(PAGE_PROBE_SCRIPT)
    if not helper.exists():
        return None

    try:
        result = subprocess.run(
            [sys.executable, str(helper)],
            capture_output=True,
            text=True,
            timeout=1.5,
            creationflags=subprocess.CREATE_NO_WINDOW if os.name == "nt" else 0,
        )
    except subprocess.TimeoutExpired:
        return None
    except Exception as e:
        print(f"[PAGE PROBE] launch failed: {e}")
        return None

    if result.returncode != 0:
        err = (result.stderr or "").strip()
        if err:
            print(f"[PAGE PROBE] helper error: {err}")
        return None

    page = (result.stdout or "").strip().lower()
    if not page or page == "none":
        return None

    return page


# -----------------------------
# Networking
# -----------------------------


async def send_json(writer: asyncio.StreamWriter, payload: dict):
    data = (json.dumps(payload, ensure_ascii=False) + "\n").encode("utf-8")
    writer.write(data)
    await writer.drain()


async def broadcast(payload: dict):
    dead = []
    for writer in clients:
        try:
            await send_json(writer, payload)
        except Exception:
            dead.append(writer)

    for writer in dead:
        clients.discard(writer)
        try:
            writer.close()
            await writer.wait_closed()
        except Exception:
            pass


async def send_current_layout(writer: asyncio.StreamWriter):
    active = is_resolve_active()
    await send_json(writer, build_layout(active))


async def handle_action(action_id: str, writer: asyncio.StreamWriter | None = None):
    if not is_resolve_active():
        print(f"[IGNORED] Resolve není aktivní: {action_id}")
        if writer:
            await send_json(writer, {
                "type": "action_result",
                "ok": False,
                "action": action_id,
                "message": "Resolve není aktivní"
            })
        return

    try:
        info = get_foreground_window_info()
        print(f"[FOCUS] title='{info['title']}' process='{info['process_name']}'")

        time.sleep(0.03)
        ok, message = execute_action(action_id)
        print(f"[OK] {message}")

        if writer:
            await send_json(writer, {
                "type": "action_result",
                "ok": ok,
                "action": action_id,
                "message": message
            })
    except Exception as e:
        print(f"[ERR] Action failed: {action_id}: {e}")
        if writer:
            await send_json(writer, {
                "type": "action_result",
                "ok": False,
                "action": action_id,
                "message": str(e)
            })


async def client_connected(reader: asyncio.StreamReader, writer: asyncio.StreamWriter):
    addr = writer.get_extra_info("peername")
    print(f"[CONNECT] {addr}")
    clients.add(writer)

    await send_current_layout(writer)

    try:
        while True:
            line = await reader.readline()
            if not line:
                break

            try:
                msg = json.loads(line.decode("utf-8").strip())
            except json.JSONDecodeError:
                print("[WARN] Invalid JSON")
                continue

            msg_type = msg.get("type")

            if msg_type == "hello":
                print(f"[HELLO] {msg}")
                await send_current_layout(writer)

            elif msg_type == "button_press":
                action = msg.get("action", "noop")
                await handle_action(action, writer)

            elif msg_type == "change_page":
                requested = msg.get("page_id")
                if get_page(requested) is not None:
                    global current_page_id
                    current_page_id = requested

                    opened = open_resolve_page(requested)
                    print(f"[PAGE] Switched to {requested}, resolve_open={opened}")

                    await broadcast(build_layout(is_resolve_active()))
                else:
                    print(f"[WARN] Unknown page requested: {requested}")
                    
            elif msg_type == "reload_profile":
                try:
                    changed = load_profile()
                    print(f"[PROFILE] Reload requested, changed={changed}")
                    await broadcast(build_layout(is_resolve_active()))
                except Exception as e:
                    print(f"[ERR] Reload failed: {e}")

            elif msg_type == "ping":
                await send_json(writer, {"type": "pong", "ts": time.time()})

            else:
                print(f"[WARN] Unknown message type: {msg_type}")

    except Exception as e:
        print(f"[ERR] Client loop error: {e}")
    finally:
        print(f"[DISCONNECT] {addr}")
        clients.discard(writer)
        writer.close()
        try:
            await writer.wait_closed()
        except Exception:
            pass


async def monitor_active_window():
    global last_active_state

    while True:
        try:
            changed = load_profile()
            active = is_resolve_active()

            if changed:
                await broadcast(build_layout(active))

            if active != last_active_state:
                last_active_state = active
                print(f"[STATE] Resolve active = {active}")
                await broadcast(build_layout(active))

        except Exception as e:
            print(f"[ERR] Monitor loop: {e}")

        await asyncio.sleep(POLL_INTERVAL)


async def monitor_resolve_page():
    global last_resolve_page, current_page_id

    page_to_layout = {
        "cut": "edit",
        "edit": "edit",
        "color": "color",
        "fairlight": "audio",
        "deliver": "deliver",
        "fusion": "color",
        "media": "edit",
    }

    while True:
        try:
            page = probe_resolve_page()

            if page != last_resolve_page:
                last_resolve_page = page
                print(f"[RESOLVE PAGE] {page}")

                mapped = page_to_layout.get(page)
                if mapped and get_page(mapped) is not None:
                    current_page_id = mapped
                    await broadcast(build_layout(is_resolve_active()))

        except Exception as e:
            print(f"[ERR] Resolve page monitor: {e}")

        await asyncio.sleep(PAGE_PROBE_INTERVAL)


async def main():
    load_profile()

    ip_guess = socket.gethostbyname(socket.gethostname())
    server = await asyncio.start_server(client_connected, HOST, PORT)

    print("=" * 60)
    print("DaVinci Resolve Touch Bridge")
    print(f"Listening on: {HOST}:{PORT}")
    print(f"Likely LAN IP: {ip_guess}:{PORT}")
    print(f"Profile: {os.path.abspath(PROFILE_PATH)}")
    print("Ctrl+C pro ukončení")
    print("=" * 60)

    async with server:
        await asyncio.gather(
            server.serve_forever(),
            monitor_active_window(),
            monitor_resolve_page(),
        )


if __name__ == "__main__":
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        print("\nUkončeno.")