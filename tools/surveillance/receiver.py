#!/usr/bin/env python3
"""Private LAN receiver and localhost-only gallery for StackChan recordings."""

from __future__ import annotations

import argparse
import asyncio
import hashlib
import html
import json
import os
import re
import socket
import sqlite3
import ssl
import sys
import threading
import time
from dataclasses import dataclass
from datetime import datetime, timezone
from pathlib import Path
from typing import Any
from zoneinfo import ZoneInfo

MAX_FRAME_BYTES = 1024 * 1024
CAPTURE_ID_RE = re.compile(r"^[0-9a-f]{32}$")
GROUP_ID_RE = re.compile(r"^[A-Za-z0-9_-]{1,48}$")
KINDS = {"motion", "scan"}
VIEWS = {"fixed", "left", "center", "right"}
STOCKHOLM = ZoneInfo("Europe/Stockholm")


class ClosingConnection(sqlite3.Connection):
    """Keep sqlite's transaction context manager, but close handles on exit."""

    def __exit__(self, exc_type, exc_value, traceback):
        try:
            return super().__exit__(exc_type, exc_value, traceback)
        finally:
            self.close()


@dataclass(frozen=True)
class FrameMetadata:
    capture_id: str
    group_id: str
    kind: str
    view: str
    sequence: int
    boot_id: str
    monotonic_ms: int
    receiver_time_ms: int = 0


def parse_frame_metadata(capture_id: str, fields: dict[str, str]) -> FrameMetadata:
    if not CAPTURE_ID_RE.fullmatch(capture_id):
        raise ValueError("invalid capture id")
    group_id = fields.get("group_id", "")
    kind = fields.get("kind", "")
    view = fields.get("view", "")
    boot_id = fields.get("boot_id", "")
    if not GROUP_ID_RE.fullmatch(group_id):
        raise ValueError("invalid group id")
    if kind not in KINDS or view not in VIEWS:
        raise ValueError("invalid capture kind or view")
    if not re.fullmatch(r"[A-Za-z0-9_-]{1,32}", boot_id):
        raise ValueError("invalid boot id")
    try:
        sequence = int(fields.get("sequence", ""))
        monotonic_ms = int(fields.get("monotonic_ms", ""))
        receiver_time_ms = int(fields.get("receiver_time_ms", "0") or "0")
    except ValueError as exc:
        raise ValueError("invalid sequence or monotonic time") from exc
    if not 0 <= sequence <= 255 or not 0 <= monotonic_ms < 2**63 or not 0 <= receiver_time_ms < 2**63:
        raise ValueError("sequence or monotonic time out of range")
    return FrameMetadata(capture_id, group_id, kind, view, sequence, boot_id, monotonic_ms, receiver_time_ms)


def utc_now_ms() -> int:
    return time.time_ns() // 1_000_000


def stockholm_time(epoch_ms: int) -> str:
    return datetime.fromtimestamp(epoch_ms / 1000, timezone.utc).astimezone(STOCKHOLM).strftime(
        "%Y-%m-%d %H:%M:%S %Z"
    )


class RecordingStore:
    """Owns only this application's recording directory and SQLite index."""

    def __init__(self, data_dir: Path, retention_days: int = 30, max_bytes: int = 5_000_000_000):
        self.data_dir = data_dir
        self.recordings_dir = data_dir / "recordings"
        self.staging_dir = data_dir / ".staging"
        self.db_path = data_dir / "index.sqlite3"
        self.retention_days = retention_days
        self.max_bytes = max_bytes
        self.disk_error = ""
        self.last_heartbeat: dict[str, Any] | None = None
        self.last_heartbeat_received_ms = 0
        self._write_lock = threading.RLock()
        self._initialize()

    def _connect(self) -> sqlite3.Connection:
        conn = sqlite3.connect(self.db_path, timeout=5, factory=ClosingConnection)
        conn.row_factory = sqlite3.Row
        conn.execute("PRAGMA foreign_keys=ON")
        conn.execute("PRAGMA busy_timeout=5000")
        conn.execute("PRAGMA synchronous=FULL")
        return conn

    def _initialize(self) -> None:
        self.data_dir.mkdir(parents=True, exist_ok=True)
        self.recordings_dir.mkdir(parents=True, exist_ok=True)
        self.staging_dir.mkdir(parents=True, exist_ok=True)
        with self._connect() as conn:
            conn.execute("PRAGMA journal_mode=WAL")
            conn.execute(
                """CREATE TABLE IF NOT EXISTS frames (
                    capture_id TEXT PRIMARY KEY,
                    group_id TEXT NOT NULL,
                    kind TEXT NOT NULL,
                    view TEXT NOT NULL,
                    sequence INTEGER NOT NULL,
                    boot_id TEXT NOT NULL,
                    monotonic_ms INTEGER NOT NULL,
                    received_ms INTEGER NOT NULL,
                    captured_ms INTEGER NOT NULL,
                    size INTEGER NOT NULL,
                    sha256 TEXT NOT NULL,
                    path TEXT NOT NULL
                )"""
            )
            conn.execute("CREATE INDEX IF NOT EXISTS frames_group_idx ON frames(group_id, received_ms)")
            conn.execute(
                """CREATE TABLE IF NOT EXISTS scans (
                    group_id TEXT PRIMARY KEY,
                    status TEXT NOT NULL,
                    started_ms INTEGER NOT NULL,
                    updated_ms INTEGER NOT NULL,
                    message TEXT NOT NULL DEFAULT ''
                )"""
            )

    def _estimated_capture_time(self, metadata: FrameMetadata, received_ms: int) -> int:
        if metadata.receiver_time_ms:
            return metadata.receiver_time_ms
        heartbeat = self.last_heartbeat
        if heartbeat and heartbeat.get("boot_id") == metadata.boot_id:
            anchor_mono = heartbeat.get("monotonic_ms")
            if isinstance(anchor_mono, int) and anchor_mono >= metadata.monotonic_ms:
                return self.last_heartbeat_received_ms - (anchor_mono - metadata.monotonic_ms)
        return received_ms

    def add_frame(self, metadata: FrameMetadata, jpeg: bytes, received_ms: int | None = None) -> tuple[str, bool]:
        with self._write_lock:
            return self._add_frame(metadata, jpeg, received_ms)

    def _add_frame(self, metadata: FrameMetadata, jpeg: bytes, received_ms: int | None = None) -> tuple[str, bool]:
        """Commit a JPEG and its index row before returning success."""
        if self.disk_error:
            raise OSError(self.disk_error)
        if not jpeg or len(jpeg) > MAX_FRAME_BYTES or not jpeg.startswith(b"\xff\xd8") or not jpeg.endswith(b"\xff\xd9"):
            raise ValueError("invalid JPEG")
        received_ms = received_ms if received_ms is not None else utc_now_ms()
        digest = hashlib.sha256(jpeg).hexdigest()
        final_path = self.recordings_dir / metadata.group_id / f"{metadata.sequence:03d}_{metadata.capture_id}.jpg"
        relative_path = str(final_path.relative_to(self.data_dir))
        temp_path: Path | None = None
        try:
            with self._connect() as conn:
                existing = conn.execute(
                    "SELECT sha256 FROM frames WHERE capture_id=?", (metadata.capture_id,)
                ).fetchone()
                if existing:
                    if existing["sha256"] != digest:
                        raise FileExistsError("capture id already contains different image data")
                    return "duplicate", True

                final_path.parent.mkdir(parents=True, exist_ok=True)
                temp_path = self.staging_dir / f"{metadata.capture_id}.{os.getpid()}.tmp"
                with open(temp_path, "xb") as output:
                    output.write(jpeg)
                    output.flush()
                    os.fsync(output.fileno())
                os.replace(temp_path, final_path)
                temp_path = None
                dir_fd = os.open(final_path.parent, os.O_RDONLY)
                try:
                    os.fsync(dir_fd)
                finally:
                    os.close(dir_fd)

                conn.execute("BEGIN IMMEDIATE")
                conn.execute(
                    """INSERT INTO frames(capture_id,group_id,kind,view,sequence,boot_id,
                       monotonic_ms,received_ms,captured_ms,size,sha256,path)
                       VALUES(?,?,?,?,?,?,?,?,?,?,?,?)""",
                    (metadata.capture_id, metadata.group_id, metadata.kind, metadata.view,
                     metadata.sequence, metadata.boot_id, metadata.monotonic_ms, received_ms,
                     self._estimated_capture_time(metadata, received_ms), len(jpeg), digest, relative_path),
                )
                conn.commit()
            self.apply_retention(received_ms)
            if self.disk_error:
                raise OSError(self.disk_error)
            return "stored", False
        except FileExistsError:
            raise
        except (OSError, sqlite3.Error) as exc:
            self.disk_error = f"recording storage failed: {exc}"
            if final_path.exists():
                try:
                    with self._connect() as conn:
                        indexed = conn.execute("SELECT 1 FROM frames WHERE capture_id=?", (metadata.capture_id,)).fetchone()
                    if not indexed:
                        final_path.unlink(missing_ok=True)
                except (OSError, sqlite3.Error):
                    pass
            try:
                final_path.parent.rmdir()
            except OSError:
                pass
            raise OSError(self.disk_error) from exc
        finally:
            if temp_path is not None:
                try:
                    temp_path.unlink(missing_ok=True)
                except OSError:
                    pass

    def apply_retention(self, now_ms: int | None = None) -> int:
        with self._write_lock:
            return self._apply_retention(now_ms)

    def _apply_retention(self, now_ms: int | None = None) -> int:
        """Delete complete, oldest groups until both retention limits are met."""
        if self.disk_error:
            return 0
        now_ms = now_ms if now_ms is not None else utc_now_ms()
        cutoff_ms = now_ms - self.retention_days * 24 * 60 * 60 * 1000
        deleted = 0
        moved_to_trash: list[tuple[Path, Path]] = []
        try:
            with self._connect() as conn:
                groups = conn.execute(
                    "SELECT group_id,MIN(received_ms) AS oldest,SUM(size) AS bytes FROM frames GROUP BY group_id ORDER BY oldest"
                ).fetchall()
                total_bytes = sum(int(row["bytes"]) for row in groups)
                for group in groups:
                    expired = int(group["oldest"]) < cutoff_ms
                    over_size = total_bytes > self.max_bytes
                    if not expired and not over_size:
                        continue
                    rows = conn.execute("SELECT capture_id,path,size FROM frames WHERE group_id=?", (group["group_id"],)).fetchall()
                    # Move the complete group out of its visible path first.
                    # If any move or SQLite commit fails, restore every file.
                    for row in rows:
                        original = self.data_dir / row["path"]
                        if original.exists():
                            trash = self.staging_dir / f"delete_{row['capture_id']}.jpg"
                            os.replace(original, trash)
                            moved_to_trash.append((original, trash))
                    conn.execute("DELETE FROM frames WHERE group_id=?", (group["group_id"],))
                    conn.commit()
                    committed_trash = moved_to_trash
                    moved_to_trash = []
                    for _, trash in committed_trash:
                        trash.unlink(missing_ok=True)
                    total_bytes -= int(group["bytes"])
                    deleted += len(rows)
                    folder = self.recordings_dir / group["group_id"]
                    try:
                        folder.rmdir()
                    except OSError:
                        pass
            return deleted
        except (OSError, sqlite3.Error) as exc:
            for original, trash in reversed(moved_to_trash):
                try:
                    original.parent.mkdir(parents=True, exist_ok=True)
                    os.replace(trash, original)
                except OSError:
                    pass
            self.disk_error = f"retention failed: {exc}"
            return deleted

    def record_heartbeat(self, payload: dict[str, Any], received_ms: int | None = None) -> dict[str, Any]:
        received_ms = received_ms if received_ms is not None else utc_now_ms()
        with self._write_lock:
            self.last_heartbeat = payload
            self.last_heartbeat_received_ms = received_ms
            group_id = payload.get("scan_group_id", "")
            scan_status = payload.get("scan_status", "")
            scan_message = str(payload.get("scan_message", ""))[:160]
            if (not self.disk_error and isinstance(group_id, str) and GROUP_ID_RE.fullmatch(group_id) and
                    isinstance(scan_status, str) and scan_status in {"scanning", "complete", "failed"}):
                try:
                    with self._connect() as conn:
                        previous = conn.execute(
                            "SELECT status,updated_ms,message FROM scans WHERE group_id=?", (group_id,)
                        ).fetchone()
                        should_write = previous is None or previous["status"] != scan_status or previous["message"] != scan_message
                        if scan_status == "scanning" and previous is not None:
                            should_write = should_write or received_ms - int(previous["updated_ms"]) >= 60_000
                        if should_write:
                            started_ms = int(previous["updated_ms"]) if previous and previous["status"] == "scanning" else received_ms
                            if previous:
                                conn.execute(
                                    "UPDATE scans SET status=?,updated_ms=?,message=? WHERE group_id=?",
                                    (scan_status, received_ms, scan_message, group_id),
                                )
                            else:
                                conn.execute(
                                    "INSERT INTO scans(group_id,status,started_ms,updated_ms,message) VALUES(?,?,?,?,?)",
                                    (group_id, scan_status, started_ms, received_ms, scan_message),
                                )
                except sqlite3.Error as exc:
                    self.disk_error = f"scan status storage failed: {exc}"
        return {
            "receiver_time_ms": received_ms,
            "storage_ok": not self.disk_error,
            "storage_error": self.disk_error,
            "stored_count": self.count(),
        }

    def count(self) -> int:
        try:
            with self._connect() as conn:
                return int(conn.execute("SELECT COUNT(*) FROM frames").fetchone()[0])
        except sqlite3.Error:
            return 0

    def gallery_status(self, now_ms: int | None = None) -> dict[str, Any]:
        now_ms = now_ms if now_ms is not None else utc_now_ms()
        heartbeat_age_ms = now_ms - self.last_heartbeat_received_ms if self.last_heartbeat_received_ms else None
        heartbeat = self.last_heartbeat or {}
        return {
            "connected": heartbeat_age_ms is not None and heartbeat_age_ms <= 15_000,
            "heartbeat_age_ms": heartbeat_age_ms,
            "capture_state": str(heartbeat.get("state", "unknown")),
            "device_status": str(heartbeat.get("status", "")),
            "queued_images": int(heartbeat.get("queued_images", 0) or 0),
            "queued_bytes": int(heartbeat.get("queued_bytes", 0) or 0),
            "lost_images": int(heartbeat.get("lost_images", 0) or 0),
            "last_confirmed_save": self.latest_frame(),
            "stored_count": self.count(),
            "storage_ok": not self.disk_error,
            "storage_error": self.disk_error,
        }

    def latest_frame(self) -> dict[str, Any] | None:
        try:
            with self._connect() as conn:
                row = conn.execute(
                    "SELECT capture_id,kind,view,sequence,captured_ms,received_ms,size FROM frames ORDER BY received_ms DESC LIMIT 1"
                ).fetchone()
                return dict(row) if row else None
        except sqlite3.Error:
            return None

    def recent_frames(self, limit: int = 200) -> list[dict[str, Any]]:
        with self._connect() as conn:
            return [dict(row) for row in conn.execute(
                "SELECT capture_id,group_id,kind,view,sequence,captured_ms,received_ms,size FROM frames ORDER BY received_ms DESC LIMIT ?",
                (limit,),
            )]

    def recent_scans(self, limit: int = 50) -> list[dict[str, Any]]:
        with self._connect() as conn:
            return [dict(row) for row in conn.execute(
                "SELECT group_id,status,started_ms,updated_ms,message FROM scans ORDER BY started_ms DESC LIMIT ?",
                (limit,),
            )]

    def frame_path(self, capture_id: str) -> Path | None:
        if not CAPTURE_ID_RE.fullmatch(capture_id):
            return None
        with self._connect() as conn:
            row = conn.execute("SELECT path FROM frames WHERE capture_id=?", (capture_id,)).fetchone()
        return self.data_dir / row["path"] if row else None


def gallery_html() -> str:
    return """<!doctype html><html lang="en"><meta charset="utf-8"><meta name="viewport" content="width=device-width">
<title>StackChan Surveillance</title><style>
body{font:16px system-ui,sans-serif;max-width:1100px;margin:24px auto;padding:0 16px;background:#101719;color:#eef4f5}
.status{padding:12px;border-radius:8px;background:#26383b;margin:12px 0}.bad{color:#ffb4a8}.ok{color:#9ee6b5}
.grid{display:grid;grid-template-columns:repeat(auto-fill,minmax(200px,1fr));gap:12px}.card{background:#1a272a;padding:8px;border-radius:8px}
img{width:100%;height:auto;display:block}.muted{color:#a7b5b8;font-size:13px}
</style><h1>StackChan Surveillance</h1><section id="status" class="status">Loading receiver status…</section>
<h2>Recent scans</h2><section id="scans" class="grid"></section>
<h2>Stored recordings <span class="muted">(Stockholm time)</span></h2><section id="frames" class="grid"></section>
<script>
const esc=s=>String(s??'').replace(/[&<>\"']/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;','\"':'&quot;',"'":'&#39;'}[c]));
async function refresh(){try{const s=await(await fetch('/status',{cache:'no-store'})).json();
document.querySelector('#status').innerHTML=`Device: <b class="${s.connected?'ok':'bad'}">${s.connected?'connected':'disconnected'}</b> · Capture: ${esc(s.capture_state)} · Stored: ${s.stored_count} · Queued: ${s.queued_images} / ${s.queued_bytes} bytes · Lost: ${s.lost_images}<br>Device status: ${s.device_status?esc(s.device_status):'ready'}<br>Storage: <span class="${s.storage_ok?'ok':'bad'}">${s.storage_ok?'healthy':esc(s.storage_error)}</span><br>Last confirmed save: ${s.last_confirmed_save?esc(new Date(s.last_confirmed_save.captured_ms).toLocaleString('en-GB',{timeZone:'Europe/Stockholm'})):'none'}`;
const scans=await(await fetch('/scans',{cache:'no-store'})).json();document.querySelector('#scans').innerHTML=scans.map(s=>`<article class="card"><b>${esc(s.status)}</b> · ${esc(s.group_id.slice(0,8))}<div>${esc(s.message)}</div><div class="muted">${esc(new Date(s.started_ms).toLocaleString('en-GB',{timeZone:'Europe/Stockholm'}))}</div></article>`).join('');
const frames=await(await fetch('/frames',{cache:'no-store'})).json();document.querySelector('#frames').innerHTML=frames.map(f=>`<article class="card"><img loading="lazy" src="/image/${esc(f.capture_id)}"><div>${esc(f.kind)} · ${esc(f.view)} · #${esc(f.sequence)}</div><div class="muted">${esc(new Date(f.captured_ms).toLocaleString('en-GB',{timeZone:'Europe/Stockholm'}))}</div></article>`).join('');
}catch(e){document.querySelector('#status').textContent='Gallery could not read receiver status.'}}
refresh();setInterval(refresh,5000);
</script></html>"""


def create_apps(store: RecordingStore, token: str, certificate: Path, private_key: Path):
    try:
        from aiohttp import web
    except ImportError as exc:
        raise RuntimeError("Install receiver dependencies with: python3 -m pip install -r requirements.txt") from exc

    def authorized(request) -> bool:
        supplied = request.headers.get("X-StackChan-Token", "")
        return bool(token) and __import__("hmac").compare_digest(supplied, token)

    async def put_frame(request):
        if not authorized(request):
            raise web.HTTPUnauthorized()
        capture_id = request.match_info["capture_id"]
        try:
            metadata = parse_frame_metadata(capture_id, {
                "group_id": request.headers.get("X-Capture-Group", ""),
                "kind": request.headers.get("X-Capture-Kind", ""),
                "view": request.headers.get("X-Capture-View", ""),
                "sequence": request.headers.get("X-Capture-Sequence", ""),
                "boot_id": request.headers.get("X-Device-Boot", ""),
                "monotonic_ms": request.headers.get("X-Capture-Monotonic-Ms", ""),
                "receiver_time_ms": request.headers.get("X-Capture-Receiver-Time-Ms", "0"),
            })
        except ValueError as exc:
            raise web.HTTPBadRequest(text=str(exc))
        if request.content_type != "image/jpeg":
            raise web.HTTPUnsupportedMediaType(text="send image/jpeg")
        if request.content_length is not None and request.content_length > MAX_FRAME_BYTES:
            raise web.HTTPRequestEntityTooLarge(max_size=MAX_FRAME_BYTES, actual_size=request.content_length)
        jpeg = await request.content.read(MAX_FRAME_BYTES + 1)
        if len(jpeg) > MAX_FRAME_BYTES:
            raise web.HTTPRequestEntityTooLarge(max_size=MAX_FRAME_BYTES, actual_size=len(jpeg))
        if store.disk_error:
            raise web.HTTPInsufficientStorage(text=store.disk_error)
        try:
            result, duplicate = await asyncio.to_thread(store.add_frame, metadata, jpeg)
        except FileExistsError as exc:
            raise web.HTTPConflict(text=str(exc))
        except ValueError as exc:
            raise web.HTTPBadRequest(text=str(exc))
        except OSError as exc:
            raise web.HTTPInsufficientStorage(text=str(exc))
        return web.json_response({"capture_id": capture_id, "stored": result == "stored", "duplicate": duplicate})

    async def heartbeat(request):
        if not authorized(request):
            raise web.HTTPUnauthorized()
        try:
            payload = await request.json()
        except (json.JSONDecodeError, UnicodeDecodeError):
            raise web.HTTPBadRequest(text="invalid JSON")
        required = {"boot_id": str, "monotonic_ms": int, "state": str}
        if not isinstance(payload, dict) or any(not isinstance(payload.get(k), t) for k, t in required.items()):
            raise web.HTTPBadRequest(text="missing heartbeat fields")
        return web.json_response(store.record_heartbeat(payload))

    api = web.Application(client_max_size=MAX_FRAME_BYTES + 1024)
    api.router.add_put("/v1/frames/{capture_id}", put_frame)
    api.router.add_post("/v1/heartbeat", heartbeat)

    gallery = web.Application()
    async def index(_):
        return web.Response(text=gallery_html(), content_type="text/html")
    async def status(_):
        return web.json_response(store.gallery_status())
    async def frames(_):
        result = []
        for frame in await asyncio.to_thread(store.recent_frames):
            frame["time"] = stockholm_time(frame["captured_ms"])
            result.append(frame)
        return web.json_response(result)
    async def scans(_):
        return web.json_response(await asyncio.to_thread(store.recent_scans))
    async def image(request):
        path = store.frame_path(request.match_info["capture_id"])
        if not path or not path.is_file():
            raise web.HTTPNotFound()
        return web.FileResponse(path, headers={"Cache-Control": "private, max-age=3600"})
    gallery.router.add_get("/", index)
    gallery.router.add_get("/status", status)
    gallery.router.add_get("/frames", frames)
    gallery.router.add_get("/scans", scans)
    gallery.router.add_get("/image/{capture_id}", image)

    context = ssl.create_default_context(ssl.Purpose.CLIENT_AUTH)
    context.minimum_version = ssl.TLSVersion.TLSv1_2
    context.load_cert_chain(str(certificate), str(private_key))
    return api, gallery, context


def load_config(path: Path) -> dict[str, Any]:
    config = json.loads(path.read_text(encoding="utf-8"))
    required = ("pairing_token", "certificate", "private_key", "data_dir")
    if any(not config.get(key) for key in required):
        raise ValueError(f"receiver config must include {', '.join(required)}")
    return config


async def run(args) -> None:
    try:
        from aiohttp import web
        from zeroconf import IPVersion, ServiceInfo, Zeroconf
    except ImportError as exc:
        raise RuntimeError("Install receiver dependencies with: python3 -m pip install -r requirements.txt") from exc

    config = load_config(args.config)
    store = RecordingStore(Path(config["data_dir"]))
    api, gallery, tls = create_apps(store, config["pairing_token"], Path(config["certificate"]), Path(config["private_key"]))
    api_runner = web.AppRunner(api, access_log=None)
    gallery_runner = web.AppRunner(gallery, access_log=None)
    await api_runner.setup()
    await gallery_runner.setup()
    await web.TCPSite(api_runner, args.api_host, args.api_port, ssl_context=tls).start()
    await web.TCPSite(gallery_runner, "127.0.0.1", args.gallery_port).start()

    zc = Zeroconf(ip_version=IPVersion.V4Only)
    host_name = "stackchan-surveillance.local."
    ipv4 = config.get("mdns_ipv4")
    if not ipv4:
        probe = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        try:
            probe.connect(("192.0.2.1", 9))
            ipv4 = probe.getsockname()[0]
        except OSError:
            ipv4 = "127.0.0.1"
        finally:
            probe.close()
    info = ServiceInfo(
        "_https._tcp.local.", "StackChan Surveillance._https._tcp.local.",
        addresses=[socket.inet_aton(ipv4)],
        port=args.api_port, properties={b"path": b"/v1"}, server=host_name,
    )
    zc.register_service(info)
    print(f"API https://stackchan-surveillance.local:{args.api_port}; gallery http://127.0.0.1:{args.gallery_port}", flush=True)
    try:
        await asyncio.Event().wait()
    finally:
        zc.unregister_service(info)
        zc.close()
        await api_runner.cleanup()
        await gallery_runner.cleanup()


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--config", type=Path, default=Path.home() / ".config/stackchan-surveillance/receiver.json")
    parser.add_argument("--api-host", default="0.0.0.0")
    parser.add_argument("--api-port", type=int, default=8443)
    parser.add_argument("--gallery-port", type=int, default=8765)
    args = parser.parse_args(argv)
    try:
        asyncio.run(run(args))
    except KeyboardInterrupt:
        pass
    except Exception as exc:
        print(f"receiver failed: {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
