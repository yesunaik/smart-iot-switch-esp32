#!/usr/bin/env python3
"""
SmartSwitch API Server  —  v5.0.0  PRODUCTION
==============================================
  CHANGES FROM v4.9.6:
    - MySQL database integration added.
    - Device MAC address persisted to `devices` table on every heartbeat.
    - New Swagger endpoints under  📦 Devices (DB)  tag:
        GET  /devices              — list all registered devices
        GET  /devices/{mac}        — single device detail
        DELETE /devices/{mac}      — remove a device record
        GET  /devices/{mac}/relay  — relay status from DB
    - Relay commands now update `last_cmd` / `last_cmd_at` in DB.
    - db_init() auto-creates the `devices` table if it does not exist.
    - Firmware must send "mac" field in heartbeat (v46+).

  !! EDIT DB_CONFIG BELOW TO MATCH YOUR SETUP !!
  ─────────────────────────────────────────────
  DB host     localhost
  DB port     3306
  DB user     root
  DB password your_password
  DB name     smartswitch
  ─────────────────────────────────────────────

  ESP32 HTTP polling  →  POST /device/heartbeat every 5s
  Swagger             →  http://localhost:8090/docs
"""

import socket, threading, datetime, time, signal, sys, pathlib, struct
import urllib.request, urllib.parse, os, platform
try:
    import pymysql
    import pymysql.cursors
    _HAS_PYMYSQL = True
except ImportError:
    _HAS_PYMYSQL = False
    print("[DB] WARNING: pymysql not installed — run: pip install pymysql")

try:
    import psutil
    _HAS_PSUTIL = True
except ImportError:
    _HAS_PSUTIL = False

from contextlib import asynccontextmanager
from fastapi import FastAPI, Query, Request, HTTPException, Path
from fastapi.responses import HTMLResponse, FileResponse, Response
from fastapi.middleware.cors import CORSMiddleware
from pydantic import BaseModel
from typing import Optional
import uvicorn

# ================================================================
#  DATABASE CONFIG  ← EDIT THESE
# ================================================================
DB_CONFIG = {
    "host":     "localhost",
    "port":     3306,
    "user":     "root",
    "password": "root",              # MySQL root password
    "database": "smartswitch_db",    # ← your actual database name
    "charset":  "utf8mb4",
    "connect_timeout": 5,
}

# ================================================================
#  SERVER CONFIG
# ================================================================
_server_start_time = time.time()
_request_counter   = 0

HTTP_PORT           = 8090
POLL_TIMEOUT        = 90
RESTART_GRACE_SEC   = 30
HEALTH_INTERVAL     = 300
lock                = threading.Lock()
last_heartbeat      = 0.0
_last_connected_log = 0.0

device = {
    "name": None, "ip": None, "net": None, "mac": None,
    "connected": False, "connected_at": None, "disconn_at": None,
    "last_cmd": None, "last_cmd_at": None, "last_hello": 0.0,
    "relay_state": None, "restarting": False,
    "target_ip": None, "rssi": None,
    "target_online": None, "ping_fails": 0,
    "pending_cmd": None,
    "auto_interval_ms": 120000,
    "auto_enabled": True,
    "restart_grace_until": None,
    "reg_active": False,
    "reg_name":   "",
    "reg_mac":    "",
    "reg_ip":     "",
    "reg_port":   80,
    "_pending_net":   "",
    "_pending_since": 0.0,
}

sched_lock = threading.Lock()
scheduler  = {
    "enabled": False, "command": 2, "interval_sec": 120,
    "next_fire": None, "last_fire": None, "fire_count": 0,
}

CMD_MAP = {0: "OFF", 1: "ON", 2: "RESTART"}

def log(msg):
    print(f"[{datetime.datetime.now().strftime('%H:%M:%S')}] {msg}", flush=True)


# ================================================================
#  DATABASE HELPERS
#  Tables  : smartswitch_details  (device registry — one row per device)
#            smartswitch_status   (status log     — one row per event)
#  Columns : sno, mac_address, relay_status, network, ip_address, updated_at
#  Extra columns added by db_init() if not present:
#            name, connected, first_seen, disconn_at, last_cmd, last_cmd_at
# ================================================================
def get_db():
    """Return a new pymysql connection. Caller must close it."""
    if not _HAS_PYMYSQL:
        raise RuntimeError("pymysql not installed")
    return pymysql.connect(**DB_CONFIG, cursorclass=pymysql.cursors.DictCursor)


def _add_column_if_missing(cur, table: str, column: str, definition: str):
    """ALTER TABLE only if the column does not already exist."""
    cur.execute("""
        SELECT COUNT(*) AS cnt
        FROM information_schema.COLUMNS
        WHERE TABLE_SCHEMA = %s
          AND TABLE_NAME   = %s
          AND COLUMN_NAME  = %s
    """, (DB_CONFIG["database"], table, column))
    if cur.fetchone()["cnt"] == 0:
        cur.execute(f"ALTER TABLE `{table}` ADD COLUMN `{column}` {definition}")
        log(f"[DB] Added column `{column}` to `{table}`")


def db_init():
    """
    Extend existing smartswitch_details and smartswitch_status tables
    with the extra columns the server needs. Safe to run on every boot —
    columns are only added if they are missing.
    """
    if not _HAS_PYMYSQL:
        log("[DB] pymysql missing — skipping db_init()")
        return

    # Extra columns we need beyond the original schema
    extra_cols = [
        ("name",        "VARCHAR(100) DEFAULT NULL"),
        ("connected",   "TINYINT(1)   DEFAULT 0"),
        ("first_seen",  "DATETIME     DEFAULT NULL"),
        ("disconn_at",  "DATETIME     DEFAULT NULL"),
        ("last_cmd",    "VARCHAR(20)  DEFAULT NULL"),
        ("last_cmd_at", "DATETIME     DEFAULT NULL"),
    ]

    try:
        conn = get_db()
        with conn:
            with conn.cursor() as cur:
                for col, defn in extra_cols:
                    _add_column_if_missing(cur, "smartswitch_details", col, defn)
                    _add_column_if_missing(cur, "smartswitch_status",  col, defn)
            conn.commit()
            # Drop UNIQUE key on smartswitch_status.mac_address
            # (it's a history table — must allow multiple rows per device)
            try:
                cur.execute("""
                    ALTER TABLE smartswitch_status
                    DROP INDEX mac_address
                """)
                log("[DB] Dropped UNIQUE index on smartswitch_status.mac_address")
            except Exception:
                pass  # already dropped or doesn't exist — fine

        log("[DB] All tables ready")
    except Exception as e:
        log(f"[DB] db_init error: {e}")


def db_upsert_device(mac: str, data: dict):
    """
    Write heartbeat data into ALL THREE tables simultaneously:

    1. devices            — full detail, one row per device (upsert)
    2. smartswitch_details — registry row, one per device (upsert)
    3. smartswitch_status  — history log, new row every heartbeat (insert)
    """
    if not _HAS_PYMYSQL or not mac or mac == "00:00:00:00:00:00":
        return

    name      = data.get("name") or ""
    ip        = data.get("ip")   or ""
    net       = data.get("net")  or ""
    relay_int = 1 if data.get("relay") else 0
    relay_str = "ON" if data.get("relay") else "OFF"

    try:
        conn = get_db()
        with conn:
            with conn.cursor() as cur:

                # ── TABLE 1: devices (full schema, one row per device) ──
                cur.execute("""
                    INSERT INTO devices
                        (mac, name, ip, net_type, relay_state,
                         rssi, auto_interval_ms, auto_enabled,
                         target_ip, ping_fails, target_online,
                         connected, first_seen, last_seen)
                    VALUES
                        (%s, %s, %s, %s, %s,
                         %s, %s, %s,
                         %s, %s, %s,
                         1, NOW(), NOW())
                    ON DUPLICATE KEY UPDATE
                        name             = VALUES(name),
                        ip               = VALUES(ip),
                        net_type         = VALUES(net_type),
                        relay_state      = VALUES(relay_state),
                        rssi             = VALUES(rssi),
                        auto_interval_ms = VALUES(auto_interval_ms),
                        auto_enabled     = VALUES(auto_enabled),
                        target_ip        = VALUES(target_ip),
                        ping_fails       = VALUES(ping_fails),
                        target_online    = VALUES(target_online),
                        connected        = 1,
                        disconn_at       = NULL,
                        last_seen        = NOW()
                """, (
                    mac, name, ip, net, relay_int,
                    data.get("rssi") or None,
                    int(data.get("autoIntervalMs", 120000)),
                    1 if data.get("autoEnabled", True) else 0,
                    data.get("targetIP") or None,
                    int(data.get("pingFails", 0)),
                    1 if data.get("targetOnline") else 0 if data.get("targetIP") else None,
                ))

                # ── TABLE 2: smartswitch_details (one row per device) ──
                cur.execute("""
                    INSERT INTO smartswitch_details
                        (mac_address, device_name, name, connected, first_seen)
                    VALUES (%s, %s, %s, 1, NOW())
                    ON DUPLICATE KEY UPDATE
                        device_name = VALUES(device_name),
                        name        = VALUES(name),
                        connected   = 1,
                        disconn_at  = NULL
                """, (mac, name, name))

                # ── TABLE 3: smartswitch_status (new row every heartbeat) ──
                cur.execute("""
                    INSERT INTO smartswitch_status
                        (mac_address, relay_status, network, ip_address,
                         name, connected, updated_at)
                    VALUES (%s, %s, %s, %s, %s, 1, NOW())
                """, (mac, relay_str, net, ip, name))

            conn.commit()
        pass  # stored silently — connect log handles the noise
    except Exception as e:
        log(f"[DB] upsert error: {e}")


def db_mark_disconnected(mac: str):
    """Mark device offline in smartswitch_details."""
    if not _HAS_PYMYSQL or not mac or mac == "00:00:00:00:00:00":
        return
    try:
        conn = get_db()
        with conn:
            with conn.cursor() as cur:
                cur.execute("""
                    UPDATE devices
                    SET connected=0, disconn_at=NOW()
                    WHERE mac=%s
                """, (mac,))
                cur.execute("""
                    UPDATE smartswitch_details
                    SET connected=0, disconn_at=NOW()
                    WHERE mac_address=%s
                """, (mac,))
                cur.execute("""
                    INSERT INTO smartswitch_status
                        (mac_address, relay_status, network, ip_address,
                         name, connected, updated_at)
                    SELECT mac,
                           CASE WHEN relay_state=1 THEN 'ON' ELSE 'OFF' END,
                           net_type, ip, name, 0, NOW()
                    FROM devices WHERE mac=%s
                """, (mac,))
            conn.commit()
    except Exception as e:
        log(f"[DB] mark_disconnected error: {e}")


def db_update_last_cmd(mac: str, cmd: str):
    """Write last_cmd into smartswitch_details after a relay command."""
    if not _HAS_PYMYSQL or not mac or mac == "00:00:00:00:00:00":
        return
    try:
        relay_str = "ON" if cmd == "ON" else "OFF"
        conn = get_db()
        with conn:
            with conn.cursor() as cur:
                # devices table
                cur.execute("""
                    UPDATE devices
                    SET last_cmd=%s, last_cmd_at=NOW(),
                        relay_state=%s
                    WHERE mac=%s
                """, (cmd, 1 if cmd=="ON" else 0, mac))
                # smartswitch_details table
                cur.execute("""
                    UPDATE smartswitch_details
                    SET last_cmd=%s, last_cmd_at=NOW()
                    WHERE mac_address=%s
                """, (cmd, mac))
                # smartswitch_status — append command event
                cur.execute("""
                    INSERT INTO smartswitch_status
                        (mac_address, relay_status, network, ip_address,
                         name, connected, updated_at)
                    SELECT mac, %s, net_type, ip, name, connected, NOW()
                    FROM devices WHERE mac=%s
                """, (relay_str if cmd in ("ON","OFF") else "OFF", mac))
            conn.commit()
    except Exception as e:
        log(f"[DB] update_last_cmd error: {e}")


def db_get_all_devices() -> list:
    """Return all rows from smartswitch_details ordered by latest first."""
    if not _HAS_PYMYSQL:
        return []
    try:
        conn = get_db()
        with conn:
            with conn.cursor() as cur:
                cur.execute("""
                    SELECT id, mac, name, ip, net_type,
                           relay_state, connected,
                           first_seen, last_seen, disconn_at,
                           last_cmd, last_cmd_at,
                           auto_interval_ms, auto_enabled,
                           target_ip, ping_fails, target_online
                    FROM devices
                    ORDER BY last_seen DESC
                """)
                rows = cur.fetchall()
        for r in rows:
            for k in ("first_seen", "last_seen", "disconn_at", "last_cmd_at"):
                if r.get(k):
                    r[k] = r[k].strftime("%Y-%m-%d %H:%M:%S")
        return rows
    except Exception as e:
        log(f"[DB] get_all_devices error: {e}")
        return []


def db_get_device(mac: str) -> dict | None:
    """Return one device row from smartswitch_details by MAC."""
    if not _HAS_PYMYSQL:
        return None
    try:
        conn = get_db()
        with conn:
            with conn.cursor() as cur:
                cur.execute("""
                    SELECT id, mac, name, ip, net_type,
                           relay_state, connected,
                           first_seen, last_seen, disconn_at,
                           last_cmd, last_cmd_at,
                           auto_interval_ms, auto_enabled,
                           target_ip, ping_fails, target_online
                    FROM devices WHERE mac=%s
                """, (mac,))
                row = cur.fetchone()
        if not row:
            return None
        for k in ("first_seen", "last_seen", "disconn_at", "last_cmd_at"):
            if row.get(k):
                row[k] = row[k].strftime("%Y-%m-%d %H:%M:%S")
        return row
    except Exception as e:
        log(f"[DB] get_device error: {e}")
        return None


def db_get_device_history(mac: str, limit: int = 50) -> list:
    """Return recent status rows from smartswitch_status for one device."""
    if not _HAS_PYMYSQL:
        return []
    try:
        conn = get_db()
        with conn:
            with conn.cursor() as cur:
                # smartswitch_status history if it has mac_address column
                cur.execute("""
                    SELECT sno, mac_address, relay_status, network,
                           ip_address, updated_at
                    FROM smartswitch_status
                    WHERE mac_address=%s
                    ORDER BY updated_at DESC
                    LIMIT %s
                """, (mac, limit))
                rows = cur.fetchall()
        for r in rows:
            if r.get("updated_at"):
                r["updated_at"] = r["updated_at"].strftime("%Y-%m-%d %H:%M:%S")
        return rows
    except Exception:
        return []


def db_delete_device(mac: str) -> bool:
    """Delete device from smartswitch_details (and its history from smartswitch_status)."""
    if not _HAS_PYMYSQL:
        return False
    try:
        conn = get_db()
        with conn:
            with conn.cursor() as cur:
                cur.execute("DELETE FROM devices WHERE mac=%s", (mac,))
                deleted = cur.rowcount
            conn.commit()
        return deleted > 0
    except Exception as e:
        log(f"[DB] delete_device error: {e}")
        return False


# ================================================================
#  BUSINESS LOGIC (same as v4.9.6)
# ================================================================
def rssi_label(rssi):
    if rssi is None: return "—"
    if rssi >= -55:  return f"{rssi} dBm (Excellent)"
    if rssi >= -65:  return f"{rssi} dBm (Good)"
    if rssi >= -75:  return f"{rssi} dBm (Fair)"
    if rssi >= -85:  return f"{rssi} dBm (Weak)"
    return f"{rssi} dBm (Very weak)"

def _set_restart_grace():
    device["restart_grace_until"] = time.time() + RESTART_GRACE_SEC

def send(cmd: str) -> bool:
    with lock:
        if not device["connected"]:
            return False
        device["pending_cmd"] = cmd
        now_str = datetime.datetime.now().strftime("%Y-%m-%d %H:%M:%S")
        device["last_cmd"]    = cmd
        device["last_cmd_at"] = now_str
        if   cmd == "ON":      device["relay_state"] = True;  device["restarting"] = False
        elif cmd == "OFF":     device["relay_state"] = False; device["restarting"] = False
        elif cmd == "RESTART":
            device["relay_state"] = False
            device["restarting"]  = True
            _set_restart_grace()
    return True


def direct_relay(esp_ip: str, cmd: str) -> bool:
    try:
        url = f"http://{esp_ip}:8080/relay/{cmd.lower()}"
        req = urllib.request.Request(url, method="POST")
        with urllib.request.urlopen(req, timeout=3) as resp:
            return resp.status == 200
    except Exception:
        return False


def direct_set_interval(esp_ip: str, ms: int) -> dict:
    try:
        url = f"http://{esp_ip}:8080/set-restart-interval?value={ms}"
        req = urllib.request.Request(url, method="POST")
        with urllib.request.urlopen(req, timeout=5) as resp:
            import json
            return json.loads(resp.read().decode())
    except Exception as e:
        log(f"  direct_set_interval failed: {e}")
        return None


def direct_set_enabled(esp_ip: str, value: int) -> dict:
    try:
        url = f"http://{esp_ip}:8080/set-restart-enabled?value={value}"
        req = urllib.request.Request(url, method="POST")
        with urllib.request.urlopen(req, timeout=5) as resp:
            import json
            return json.loads(resp.read().decode())
    except Exception as e:
        log(f"  direct_set_enabled failed: {e}")
        return None


def scheduler_worker():
    while True:
        time.sleep(1)
        with sched_lock:
            if not scheduler["enabled"] or scheduler["next_fire"] is None:
                continue
            if datetime.datetime.now() < scheduler["next_fire"]:
                continue
            cmd      = CMD_MAP.get(scheduler["command"], "RESTART")
            now      = datetime.datetime.now()
            scheduler["last_fire"]   = now.strftime("%Y-%m-%d %H:%M:%S")
            scheduler["fire_count"] += 1
            scheduler["next_fire"]   = now + datetime.timedelta(
                seconds=scheduler["interval_sec"])
            next_str = scheduler["next_fire"].strftime("%H:%M:%S")
        if cmd == "RESTART":
            with lock:
                _set_restart_grace()
        ok = send(cmd)
        log(f"⏰ Scheduler fired {cmd} — {'OK' if ok else 'FAILED'}  next:{next_str}")


def health_report_thread():
    time.sleep(HEALTH_INTERVAL)
    while True:
        with lock: d = dict(device)
        with sched_lock: s = dict(scheduler)
        W = 66
        def row(label, value):
            content = f"  {label:<24}: {value}"
            print(f"║{content}{' ' * max(0, W - len(content) - 2)}║")
        print()
        print("╔" + "═" * W + "╗")
        print(f"║{'  📊 HEALTH  ' + datetime.datetime.now().strftime('%Y-%m-%d %H:%M:%S'):^{W}}║")
        print("╠" + "═" * W + "╣")
        row("Status",         "🟢 ONLINE" if d["connected"] else "🔴 OFFLINE")
        row("Device",         d["name"] or "—")
        row("MAC",            d["mac"]  or "—")
        row("IP",             d["ip"]   or "—")
        row("Network",        d["net"]  or "—")
        row("Auto interval",  f"{d['auto_interval_ms']/1000:.0f}s")
        row("Auto enabled",   "YES" if d["auto_enabled"] else "NO")
        row("Last Command",   f"{d['last_cmd']} @ {d['last_cmd_at']}" if d["last_cmd"] else "—")
        row("Scheduler",      f"{'ON' if s['enabled'] else 'OFF'}  "
                              f"{CMD_MAP.get(s['command'], '?')} every {s['interval_sec']}s  "
                              f"fired:{s['fire_count']}x")
        row("pymysql",        "✓ installed" if _HAS_PYMYSQL else "✗ missing")
        print("╚" + "═" * W + "╝")
        print(flush=True)
        time.sleep(HEALTH_INTERVAL)


# ================================================================
#  FASTAPI APP + LIFESPAN
# ================================================================
@asynccontextmanager
async def lifespan(app: FastAPI):
    db_init()   # ← create table if not exists
    threading.Thread(target=scheduler_worker,     daemon=True).start()
    threading.Thread(target=health_report_thread, daemon=True).start()
    print()
    W = 66
    def b(line):
        print(f"║  {line}{' ' * max(0, W - len(line) - 2)}║")
    print("╔" + "═" * W + "╗")
    print(f"║{'  SmartSwitch Server  v5.0.0  PRODUCTION':^{W}}║")
    print("╠" + "═" * W + "╣")
    b(f"HTTP polling :  POST /device/heartbeat  (ESP32 → server every 5s)")
    b(f"Swagger      :  http://localhost:{HTTP_PORT}/docs")
    b(f"DB           :  MySQL — {DB_CONFIG['host']}:{DB_CONFIG['port']}/{DB_CONFIG['database']}")
    b(f"pymysql      :  {'installed ✓' if _HAS_PYMYSQL else 'NOT installed — run: pip install pymysql'}")
    b(f"psutil       :  {'installed ✓' if _HAS_PSUTIL else 'NOT installed — run: pip install psutil'}")
    print("╚" + "═" * W + "╝")
    print(flush=True)

    import asyncio

    async def offline_checker():
        global last_heartbeat
        while True:
            await asyncio.sleep(10)
            with lock:
                connected        = device["connected"]
                grace_until      = device["restart_grace_until"]
                name             = device["name"] or "ESP32"
                mac              = device["mac"]
                elapsed          = time.time() - last_heartbeat
                auto_interval_s  = device["auto_interval_ms"] / 1000
                auto_en          = device["auto_enabled"]

            if not connected:
                continue

            if auto_en:
                effective_timeout = max(auto_interval_s * 2 + 30, 300)
            else:
                effective_timeout = POLL_TIMEOUT

            if grace_until and time.time() < grace_until:
                continue

            if elapsed > effective_timeout:
                with lock:
                    device["connected"]           = False
                    device["disconn_at"]          = datetime.datetime.now().strftime(
                        "%Y-%m-%d %H:%M:%S")
                    device["restart_grace_until"] = None
                db_mark_disconnected(mac)   # ← persist to DB
                with lock:
                    net_type = device['net'] or ''
                    dev_ip   = device['ip']  or '?'
                if 'Ethernet' in net_type:
                    net_label = "ETH Disconnected"
                elif 'WiFi' in net_type:
                    net_label = "WiFi Disconnected"
                else:
                    net_label = "Disconnected"
                log(f"⚠️  {net_label} — {name}  "
                    f"MAC:{mac}  IP:{dev_ip}  Net:{net_type}")

    asyncio.create_task(offline_checker())
    yield


app = FastAPI(
    title="SmartSwitch Server",
    version="5.0.0",
    description="""
## VerifAI SmartSwitch — Server API

Control and monitor your ESP32 SmartSwitch device via this API.
All endpoints are accessible in **Swagger UI** — click **Try it out** on any endpoint.

### Quick start
1. `POST /device/heartbeat` — called automatically by ESP32 firmware
2. `GET /device/status` — check if device is online
3. `GET /device/control?deviceId=SmartSwitch&command=1` — turn relay ON
4. `GET /devices` — list all registered devices (from MySQL)
5. `GET /devices/{mac}` — single device detail by MAC address
""",
    lifespan=lifespan,
)

app.add_middleware(
    CORSMiddleware,
    allow_origins=["*"],
    allow_methods=["*"],
    allow_headers=["*"],
)

DASHBOARD_USER = "admin"
DASHBOARD_PASS = "admin"


# ================================================================
#  STATIC FILES
# ================================================================
@app.get("/", response_class=HTMLResponse, include_in_schema=False)
def serve_ui():
    p = pathlib.Path(__file__).parent / "index.html"
    return HTMLResponse(
        p.read_text(encoding="utf-8") if p.exists() else "<h2>index.html not found</h2>",
        status_code=200 if p.exists() else 404,
    )

@app.get("/verifaiLogo.jpg", include_in_schema=False)
def serve_logo():
    p = pathlib.Path(__file__).parent / "verifaiLogo.jpg"
    if p.exists():
        return FileResponse(str(p), media_type="image/jpeg")
    return Response(status_code=404)


# ================================================================
#  AUTH
# ================================================================
@app.get(
    "/device/auth",
    summary="Validate dashboard credentials",
    tags=["🔑 Auth"],
    response_description="Returns ok:true if credentials match",
)
def device_auth(
    user: str = Query(..., description="Username", openapi_examples={"ex": {"summary": "Example", "value": "admin"}}),
    pwd:  str = Query(..., alias="pass", description="Password", openapi_examples={"ex": {"summary": "Example", "value": "admin"}}),
):
    ok = (user == DASHBOARD_USER and pwd == DASHBOARD_PASS)
    return {"ok": ok, "user": user}


# ================================================================
#  HEARTBEAT
# ================================================================
@app.post(
    "/device/heartbeat",
    summary="ESP32 heartbeat (called by firmware every 5s)",
    tags=["📡 Device"],
    response_description="Returns pending command string or empty string",
)
async def device_heartbeat(request: Request):
    """
    Called automatically by ESP32 firmware.

    **Firmware sends:** name, ip, net, relay, rssi, targetIP, targetOnline,
    pingFails, autoIntervalMs, autoEnabled, **mac** (new in v46).

    **Server returns:** `{"cmd": "ON" | "OFF" | "RESTART" | ""}` — firmware
    executes the command immediately on receipt.
    """
    global last_heartbeat
    try:
        data = await request.json()
    except Exception:
        return {"cmd": ""}

    now_ts  = time.time()
    now_str = datetime.datetime.now().strftime("%Y-%m-%d %H:%M:%S")
    mac     = data.get("mac", "")   # ← from firmware v46+

    with lock:
        was_connected  = device["connected"]
        prev_last_beat = last_heartbeat
        prev_net       = device["net"] or ""

        device["name"]          = data.get("name", device["name"])
        device["ip"]            = data.get("ip",   device["ip"])
        device["net"]           = data.get("net",  device["net"])
        device["mac"]           = mac or device["mac"]
        device["rssi"]          = data.get("rssi") or None
        device["target_ip"]     = data.get("targetIP", device["target_ip"])
        device["target_online"] = bool(data.get("targetOnline", 0)) if data.get("targetIP") else None
        device["ping_fails"]    = data.get("pingFails", 0)
        device["connected"]     = True
        device["disconn_at"]    = None
        last_heartbeat          = now_ts

        if data.get("autoIntervalMs"):
            device["auto_interval_ms"] = int(data["autoIntervalMs"])
        if data.get("autoEnabled") is not None:
            device["auto_enabled"] = bool(data["autoEnabled"])

        if data.get("regActive") is not None:
            device["reg_active"] = bool(data["regActive"])
        if data.get("regName"):
            device["reg_name"] = data["regName"]
        if data.get("regMAC"):
            device["reg_mac"] = data["regMAC"]
        if data.get("regIP"):
            device["reg_ip"] = data["regIP"]
        if data.get("regPort"):
            device["reg_port"] = int(data["regPort"])

        new_relay = bool(data.get("relay", 0))
        device["relay_state"] = new_relay
        if new_relay:
            device["restarting"] = False
        else:
            if device["restarting"] and device["last_cmd_at"]:
                try:
                    cmd_time = datetime.datetime.strptime(
                        device["last_cmd_at"], "%Y-%m-%d %H:%M:%S")
                    if (datetime.datetime.now() - cmd_time).total_seconds() > 15:
                        device["restarting"] = False
                except Exception:
                    pass

        cmd = device["pending_cmd"]
        device["pending_cmd"] = None

    heartbeat_gap = now_ts - prev_last_beat if prev_last_beat > 0 else 999
    new_net       = data.get("net") or ""
    net_changed   = prev_net != new_net

    # ── Network change debounce — suppress flickers under 10s ────
    # Simple debounce — only suppress if gap < 10s from last success
    # This handles relay restart flicker without complex state machine
    with lock:
        if net_changed and new_net == "":
            net_changed = False  # ignore empty net during transition

    # ── Persist to MySQL ─────────────────────────────────────────
    db_upsert_device(mac, data)

   # ── Log rules ────────────────────────────────────────────────
    # ── Log rules ────────────────────────────────────────────────
    # Only log "Connected" when device was genuinely offline before.
    # heartbeat_gap > 8 is intentionally removed — relay restarts cause
    # a brief gap but the device never truly disconnected.
    if not was_connected or data.get("firstBoot", False):
        with lock:
            device["connected_at"] = now_str
        log(f"✅ Connected — {data.get('name','ESP32')}  "
            f"MAC:{mac or '?'}  IP:{data.get('ip','?')}  "
            f"Net:{data.get('net','?')}")
    elif net_changed:
        with lock:
            device["connected_at"] = now_str
        log(f"🔄 Network — {data.get('name','ESP32')}  "
            f"IP:{data.get('ip','?')}  {prev_net} → {new_net}")

    return {"cmd": cmd or ""}


# ================================================================
#  RELAY CONTROL
# ================================================================
@app.get(
    "/device/control",
    summary="Send relay command to device",
    tags=["🔌 Relay Control"],
    response_description="SUCCESS or ERROR with action performed",
)
def device_control(
    deviceId: str = Query(..., description="Device name (e.g. SmartSwitch)", openapi_examples={"ex": {"summary": "Example", "value": "SmartSwitch"}}),
    command:  int = Query(..., ge=0, le=2,
                          description="0 = OFF  |  1 = ON  |  2 = RESTART"),
):
    """
    Send a relay command via the server.

    | command | action  |
    |---------|---------|
    | 0       | Turn OFF |
    | 1       | Turn ON  |
    | 2       | Restart (OFF then ON after 3s) |

    The command is delivered to the ESP32 on its next heartbeat
    **or** pushed instantly via direct HTTP call if the device IP is known.
    """
    cmd = CMD_MAP[command]
    with lock:
        connected = device["connected"]
        name      = device["name"]
        esp_ip    = device["ip"]
        mac       = device["mac"]

    if not connected:
        return {"status": "ERROR", "message": "Device not connected", "action": cmd}
    if name and deviceId.strip() != name.strip():
        return {"status": "ERROR",
                "message": f"Device '{deviceId}' not found. Connected: '{name}'",
                "action": cmd}

    ok = False
    if esp_ip:
        ok = direct_relay(esp_ip, cmd)
    if not ok:
        ok = send(cmd)

    if ok:
        now_str = datetime.datetime.now().strftime("%Y-%m-%d %H:%M:%S")
        with lock:
            device["last_cmd"]    = cmd
            device["last_cmd_at"] = now_str
            if   cmd == "ON":      device["relay_state"] = True;  device["restarting"] = False
            elif cmd == "OFF":     device["relay_state"] = False; device["restarting"] = False
            elif cmd == "RESTART":
                device["relay_state"] = False
                device["restarting"]  = True
                _set_restart_grace()
        db_update_last_cmd(mac, cmd)   # ← persist to DB

    return {
        "status":   "SUCCESS" if ok else "ERROR",
        "message":  "Command sent" if ok else "Failed to send",
        "deviceId": deviceId,
        "command":  command,
        "action":   cmd,
    }


@app.post(
    "/relay/on",
    summary="Turn relay ON",
    tags=["🔌 Relay Control"],
)
def relay_on():
    """Turn the relay ON immediately."""
    ok = False
    with lock:
        esp_ip = device["ip"]
        mac    = device["mac"]
    if esp_ip:
        ok = direct_relay(esp_ip, "ON")
    if not ok:
        ok = send("ON")
    if ok:
        db_update_last_cmd(mac, "ON")
    return {"status": "ok" if ok else "error", "relay": "ON"}


@app.post(
    "/relay/off",
    summary="Turn relay OFF",
    tags=["🔌 Relay Control"],
)
def relay_off():
    """Turn the relay OFF immediately."""
    ok = False
    with lock:
        esp_ip = device["ip"]
        mac    = device["mac"]
    if esp_ip:
        ok = direct_relay(esp_ip, "OFF")
    if not ok:
        ok = send("OFF")
    if ok:
        db_update_last_cmd(mac, "OFF")
    return {"status": "ok" if ok else "error", "relay": "OFF"}


@app.post(
    "/relay/restart",
    summary="Restart relay (OFF → 3s delay → ON)",
    tags=["🔌 Relay Control"],
)
def relay_restart():
    """Power-cycle the relay: turns OFF, waits 3 seconds, turns back ON."""
    ok = False
    with lock:
        esp_ip = device["ip"]
        mac    = device["mac"]
    if esp_ip:
        ok = direct_relay(esp_ip, "RESTART")
    if not ok:
        ok = send("RESTART")
    if ok:
        with lock:
            _set_restart_grace()
        db_update_last_cmd(mac, "RESTART")
    return {"status": "ok" if ok else "error", "relay": "RESTARTING"}


# ================================================================
#  DEVICE STATUS
# ================================================================
@app.get(
    "/device/status",
    summary="Current live device status (in-memory)",
    tags=["📡 Device"],
)
def device_status():
    """
    Returns the **live in-memory** state of the connected device.
    This reflects the most recent heartbeat — no DB query.

    Use `GET /devices/{mac}` for the **persisted DB record**.
    """
    with lock:
        d = dict(device)
    in_grace = (d["restart_grace_until"] is not None and
                time.time() < d["restart_grace_until"])
    return {
        "connected":        d["connected"] or in_grace,
        "restarting":       d["restarting"] or in_grace,
        "name":             d["name"],
        "mac":              d["mac"],
        "ip":               d["ip"],
        "network_type":     d["net"],
        "connected_at":     d["connected_at"],
        "disconn_at":       d["disconn_at"],
        "last_command":     d["last_cmd"],
        "last_cmd_at":      d["last_cmd_at"],
        "relay_state":      d["relay_state"],
        "target_ip":        d["target_ip"],
        "target_online":    d["target_online"],
        "ping_fails":       d["ping_fails"],
        "rssi":             d["rssi"],
        "rssi_label":       rssi_label(d["rssi"]),
        "auto_interval_ms": d["auto_interval_ms"],
        "auto_enabled":     d["auto_enabled"],
        "in_restart_grace": in_grace,
        "reg_active":       d["reg_active"],
        "reg_name":         d["reg_name"],
        "reg_mac":          d["reg_mac"],
        "reg_ip":           d["reg_ip"],
        "reg_port":         d["reg_port"],
    }


# ================================================================
#  📦 DEVICES (DB) — new in v5.0.0
# ================================================================
@app.get(
    "/devices",
    summary="List all registered devices (from MySQL)",
    tags=["📦 Devices (DB)"],
    response_description="Array of all device records stored in the database",
)
def list_devices():
    """
    Returns **all devices** ever seen by the server, read from MySQL.

    Each record includes:
    - `mac` — unique device identifier (WiFi MAC address)
    - `connected` — whether the device is currently online
    - `relay_state` — last known relay state (0 = OFF, 1 = ON)
    - `first_seen` / `last_seen` — timestamps
    - `last_cmd` — last relay command sent (ON / OFF / RESTART)
    - `auto_interval_ms` — current watchdog interval
    """
    rows = db_get_all_devices()
    return {
        "count":   len(rows),
        "devices": rows,
    }


@app.get(
    "/devices/{mac}",
    summary="Get a single device by MAC address",
    tags=["📦 Devices (DB)"],
    response_description="Device record from MySQL, or 404 if not found",
)
def get_device(
    mac: str = Path(
        ...,
        description="WiFi MAC address of the device", openapi_examples={"ex": {"summary": "Example", "value": "AA:BB:CC:DD:EE:FF"}},
    )
):
    """
    Returns the **persisted DB record** for one device identified by its
    WiFi MAC address.

    The MAC is always in upper-case `AA:BB:CC:DD:EE:FF` format as reported
    by the ESP32 `WiFi.macAddress()` call.
    """
    row = db_get_device(mac.upper())
    if not row:
        raise HTTPException(status_code=404, detail=f"Device {mac} not found in database")
    return row


@app.delete(
    "/devices/{mac}",
    summary="Remove a device record from the database",
    tags=["📦 Devices (DB)"],
)
def delete_device(
    mac: str = Path(
        ...,
        description="WiFi MAC address of the device to remove", openapi_examples={"ex": {"summary": "Example", "value": "AA:BB:CC:DD:EE:FF"}},
    )
):
    """
    Deletes the device record from MySQL.

    This does **not** disconnect the physical device — it only removes
    the stored history. The device will be re-added on its next heartbeat.
    """
    ok = db_delete_device(mac.upper())
    if not ok:
        raise HTTPException(status_code=404, detail=f"Device {mac} not found")
    return {"status": "deleted", "mac": mac.upper()}


@app.get(
    "/devices/{mac}/history",
    summary="Get status history for a device (from smartswitch_status)",
    tags=["📦 Devices (DB)"],
)
def get_device_history(
    mac: str = Path(..., description="WiFi MAC address",
                    openapi_examples={"ex": {"summary": "Example", "value": "AA:BB:CC:DD:EE:FF"}}),
    limit: int = Query(50, ge=1, le=500, description="Number of rows to return"),
):
    """
    Returns recent rows from **smartswitch_status** for this device.
    Every heartbeat appends one row — full timeline of relay state,
    network type and IP address changes over time.
    """
    rows = db_get_device_history(mac.upper(), limit)
    return {"mac": mac.upper(), "count": len(rows), "history": rows}


@app.get(
    "/devices/{mac}/relay",
    summary="Get relay state for a device from DB",
    tags=["📦 Devices (DB)"],
)
def get_device_relay(
    mac: str = Path(..., description="WiFi MAC address",
                    openapi_examples={"ex": {"summary": "Example", "value": "AA:BB:CC:DD:EE:FF"}}),
):
    """
    Returns the last known relay state from smartswitch_details.
    Use GET /device/status for real-time state.
    """
    row = db_get_device(mac.upper())
    if not row:
        raise HTTPException(status_code=404, detail=f"Device {mac} not found")
    return {
        "mac_address":  row["mac_address"],
        "name":         row["name"],
        "relay_status": row["relay_status"],
        "last_cmd":     row["last_cmd"],
        "last_cmd_at":  row["last_cmd_at"],
        "connected":    bool(row["connected"]),
        "updated_at":   row["updated_at"],
    }


@app.post(
    "/devices/{mac}/relay",
    summary="Send relay command to device by MAC address",
    tags=["📦 Devices (DB)"],
)
def control_device_relay(
    mac: str = Path(..., description="WiFi MAC address",
                    openapi_examples={"ex": {"summary": "Example", "value": "AA:BB:CC:DD:EE:FF"}}),
    command: int = Query(..., ge=0, le=2,
                         description="0 = OFF  |  1 = ON  |  2 = RESTART"),
):
    """
    Control relay using the device MAC address as identifier.

    | command | action   |
    |---------|----------|
    | 0       | Turn OFF |
    | 1       | Turn ON  |
    | 2       | Restart  |
    """
    row = db_get_device(mac.upper())
    if not row:
        raise HTTPException(status_code=404, detail=f"Device {mac} not found in database")
    if not row["connected"]:
        return {"status": "ERROR",
                "message": f"Device {mac} is currently offline",
                "relay": CMD_MAP[command]}
    cmd = CMD_MAP[command]
    ok  = False
    if row.get("ip"):
        ok = direct_relay(row["ip"], cmd)
    if not ok:
        ok = send(cmd)
    if ok:
        db_update_last_cmd(mac.upper(), cmd)
        if cmd == "RESTART":
            with lock:
                _set_restart_grace()
    return {
        "status":  "SUCCESS" if ok else "ERROR",
        "mac":     mac.upper(),
        "name":    row["name"],
        "command": command,
        "action":  cmd,
        "message": "Command sent" if ok else "Failed",
    }



# ================================================================
#  AUTO-RESTART WATCHDOG
# ================================================================
@app.post(
    "/device/set-restart-interval",
    summary="Set ESP32 auto-restart interval (ms)",
    tags=["⚙️ Watchdog"],
)
def set_restart_interval(
    value: int = Query(...,
                       description="Interval in milliseconds",
                       ge=10000, le=3600000, openapi_examples={"ex": {"summary": "Example", "value": 120000}}),
):
    """
    Set how often the ESP32 automatically power-cycles the relay.

    | value (ms) | interval |
    |------------|----------|
    | 30000      | 30 seconds |
    | 60000      | 1 minute |
    | 120000     | 2 minutes (default) |
    | 300000     | 5 minutes |
    | 3600000    | 60 minutes |
    """
    with lock:
        connected = device["connected"]
        esp_ip    = device["ip"]
    if not connected or not esp_ip:
        return {"status": "ERROR", "message": "ESP32 not connected"}
    result = direct_set_interval(esp_ip, value)
    if result and result.get("status") == "ok":
        actual_ms = result.get("autoIntervalMs", value)
        with lock:
            device["auto_interval_ms"] = actual_ms
        log(f"⏱  Auto-restart interval → {actual_ms}ms ({actual_ms/60000:.1f} min)")
        return {
            "status":          "ok",
            "autoIntervalMs":  actual_ms,
            "autoIntervalSec": actual_ms // 1000,
            "message":         f"Saved to ESP32 flash. Interval: {actual_ms/1000:.0f}s",
        }
    return {"status": "ERROR", "message": "ESP32 rejected — check firmware v42+"}


@app.post(
    "/device/set-restart-enabled",
    summary="Enable or disable the auto-restart watchdog",
    tags=["⚙️ Watchdog"],
)
def set_restart_enabled(
    value: int = Query(...,
                       description="1 = enable watchdog  |  0 = disable watchdog",
                       ge=0, le=1, openapi_examples={"ex": {"summary": "Example", "value": 1}}),
):
    """
    Enable or disable the periodic auto-restart watchdog on the ESP32.

    When **disabled** (`value=0`) the relay will never auto-restart.
    When **enabled** (`value=1`) it will restart at the configured interval.
    """
    with lock:
        connected = device["connected"]
        esp_ip    = device["ip"]
    if not connected or not esp_ip:
        return {"status": "ERROR", "message": "ESP32 not connected"}
    result = direct_set_enabled(esp_ip, value)
    if result and result.get("status") == "ok":
        with lock:
            device["auto_enabled"] = bool(value)
        log(f"⏱  Auto-restart watchdog {'ENABLED' if value else 'DISABLED'}")
        return {
            "status":      "ok",
            "autoEnabled": bool(value),
            "message":     f"Watchdog {'enabled' if value else 'disabled'} on ESP32",
        }
    return {"status": "ERROR", "message": "ESP32 rejected — check firmware v42+"}


# ================================================================
#  SCHEDULER
# ================================================================
@app.post(
    "/scheduler/start",
    summary="Start the server-side scheduler",
    tags=["⏰ Scheduler"],
)
def scheduler_start(
    command:          int = Query(2,   ge=0, le=2,
                                  description="0=OFF  1=ON  2=RESTART", openapi_examples={"ex": {"summary": "Example", "value": 2}}),
    interval_seconds: int = Query(120, ge=10,
                                  description="Fire interval in seconds", openapi_examples={"ex": {"summary": "Example", "value": 120}}),
):
    """
    Start the **server-side** scheduler.

    The server will send the chosen command to the ESP32 every
    `interval_seconds` seconds, independent of the ESP32's own
    built-in watchdog.

    This is useful when you want the server to control restart timing,
    or when you want ON/OFF cycling rather than just restart.
    """
    now = datetime.datetime.now()
    nf  = now + datetime.timedelta(seconds=interval_seconds)
    with sched_lock:
        scheduler.update({
            "enabled":      True,
            "command":      command,
            "interval_sec": interval_seconds,
            "next_fire":    nf,
        })
    log(f"⏰ Scheduler STARTED — {CMD_MAP[command]} every {interval_seconds}s")
    return {
        "status":           "SUCCESS",
        "action":           CMD_MAP[command],
        "interval_seconds": interval_seconds,
        "first_fire_at":    nf.strftime("%Y-%m-%d %H:%M:%S"),
    }


@app.post(
    "/scheduler/stop",
    summary="Stop the server-side scheduler",
    tags=["⏰ Scheduler"],
)
def scheduler_stop():
    """Stop the server scheduler. Does not affect the ESP32 built-in watchdog."""
    with sched_lock:
        was   = scheduler["enabled"]
        scheduler["enabled"]   = False
        scheduler["next_fire"] = None
        count = scheduler["fire_count"]
    log(f"⏰ Scheduler STOPPED — fired {count}x")
    return {
        "status":      "SUCCESS",
        "message":     "Stopped" if was else "Was not running",
        "total_fired": count,
    }


@app.get(
    "/scheduler/status",
    summary="Server scheduler status",
    tags=["⏰ Scheduler"],
)
def scheduler_status():
    """Returns current scheduler state including countdown to next fire."""
    with sched_lock:
        e  = scheduler["enabled"]
        c  = scheduler["command"]
        iv = scheduler["interval_sec"]
        nf = scheduler["next_fire"]
        lf = scheduler["last_fire"]
        fc = scheduler["fire_count"]
    rem = max(0, round((nf - datetime.datetime.now()).total_seconds())) if e and nf else None
    return {
        "enabled":           e,
        "command":           c,
        "action":            CMD_MAP.get(c),
        "interval_seconds":  iv,
        "next_fire_at":      nf.strftime("%Y-%m-%d %H:%M:%S") if nf else None,
        "seconds_remaining": rem,
        "last_fire_at":      lf,
        "total_fired":       fc,
    }


# ================================================================
#  TARGET IP WATCHDOG
# ================================================================
@app.get(
    "/device/target-ip",
    summary="Get current target IP being monitored",
    tags=["🎯 Target IP Watchdog"],
)
def get_target_ip():
    """Returns the IP address the ESP32 is currently pinging every 30 seconds."""
    with lock:
        return {"target_ip": device["target_ip"], "connected": device["connected"]}


@app.post(
    "/device/target-ip",
    summary="Set target IP for the ping watchdog",
    tags=["🎯 Target IP Watchdog"],
)
def set_target_ip(
    ip: str = Query(..., description="IP address to monitor (empty string to clear)", openapi_examples={"ex": {"summary": "Example", "value": "192.168.1.50"}})
):
    """
    Set the IP address the ESP32 will ping every 30 seconds.

    If the target IP becomes unreachable for **3 consecutive pings**,
    the ESP32 will automatically restart the relay.

    Set `ip` to an empty string `""` to disable the watchdog.
    """
    ok = send(f"TARGET_IP|{ip}")
    with lock:
        device["target_ip"] = ip
    return {"status": "SUCCESS" if ok else "WARNING", "target_ip": ip}


# ================================================================
#  PASSWORD + HEALTH
# ================================================================
@app.post(
    "/device/change-password",
    summary="Change the server dashboard password",
    tags=["🔑 Auth"],
)
async def change_password(request: Request):
    """Change the password used to log in to the server dashboard UI."""
    global DASHBOARD_PASS
    try:
        data = await request.json()
    except Exception:
        return {"ok": False, "msg": "Invalid request"}
    current  = data.get("current", "")
    new_pass = data.get("password", "")
    if current != DASHBOARD_PASS:
        return {"ok": False, "msg": "Current password incorrect"}
    if not new_pass or len(new_pass) < 4:
        return {"ok": False, "msg": "Password too short (min 4 chars)"}
    DASHBOARD_PASS = new_pass
    log("🔑 Dashboard password changed")
    return {"ok": True}


@app.middleware("http")
async def count_requests(request: Request, call_next):
    global _request_counter
    _request_counter += 1
    return await call_next(request)


@app.get(
    "/server/health",
    summary="Server process health (CPU, RAM, uptime)",
    tags=["💻 Server"],
)
def server_health():
    """CPU, RAM, uptime and request stats for this Python server process."""
    uptime = time.time() - _server_start_time
    result = {
        "server_uptime_sec": round(uptime),
        "request_count":     _request_counter,
        "python_version":    platform.python_version(),
        "cpu_cores":         os.cpu_count(),
        "pymysql":           _HAS_PYMYSQL,
        "psutil":            _HAS_PSUTIL,
    }
    if _HAS_PSUTIL:
        proc = psutil.Process(os.getpid())
        vm   = psutil.virtual_memory()
        result.update({
            "cpu_percent":    psutil.cpu_percent(interval=None),
            "ram_percent":    vm.percent,
            "ram_used_mb":    round(vm.used  / 1048576, 1),
            "ram_total_mb":   round(vm.total / 1048576, 1),
            "process_mem_mb": round(proc.memory_info().rss / 1048576, 2),
        })
    return result


@app.get(
    "/health",
    summary="Quick health check",
    tags=["💻 Server"],
)
def health():
    """Minimal health-check endpoint — returns ok if the server is running."""
    return {
        "status":        "ok",
        "poll_timeout":  POLL_TIMEOUT,
        "restart_grace": RESTART_GRACE_SEC,
        "connected":     device["connected"],
        "mac":           device["mac"],
        "time":          datetime.datetime.now().isoformat(),
    }


def _shutdown(sig, frame):
    log("🛑 Shutting down")
    sys.exit(0)

signal.signal(signal.SIGINT,  _shutdown)
signal.signal(signal.SIGTERM, _shutdown)

if __name__ == "__main__":
    uvicorn.run(app, host="0.0.0.0", port=HTTP_PORT, access_log=False)