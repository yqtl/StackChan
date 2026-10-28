import importlib.util
import os
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

MODULE_PATH = Path(__file__).with_name("receiver.py")
SPEC = importlib.util.spec_from_file_location("surveillance_receiver", MODULE_PATH)
receiver = importlib.util.module_from_spec(SPEC)
assert SPEC.loader is not None
sys.modules[SPEC.name] = receiver
SPEC.loader.exec_module(receiver)


def metadata(capture_id, group, kind="motion", view="fixed", sequence=0, mono=1000):
    return receiver.FrameMetadata(capture_id, group, kind, view, sequence, "boot1234", mono)


def jpeg(fill):
    return b"\xff\xd8" + bytes([fill]) * 100 + b"\xff\xd9"


class RecordingStoreTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.store = receiver.RecordingStore(Path(self.temp.name), retention_days=30, max_bytes=1024)

    def tearDown(self):
        self.temp.cleanup()

    def test_idempotent_duplicate_and_conflict(self):
        meta = metadata("a" * 32, "motion_a")
        self.assertEqual(self.store.add_frame(meta, jpeg(1), 10_000), ("stored", False))
        self.assertEqual(self.store.add_frame(meta, jpeg(1), 10_001), ("duplicate", True))
        self.assertEqual(self.store.count(), 1)
        with self.assertRaises(FileExistsError):
            self.store.add_frame(meta, jpeg(2), 10_002)
        self.assertEqual(self.store.count(), 1)

    def test_invalid_jpeg_rejected(self):
        with self.assertRaises(ValueError):
            self.store.add_frame(metadata("b" * 32, "motion_b"), b"not jpeg")

    def test_partial_file_write_is_not_indexed(self):
        meta = metadata("c" * 32, "motion_c")
        with mock.patch.object(receiver.os, "replace", side_effect=OSError("simulated disk failure")):
            with self.assertRaises(OSError):
                self.store.add_frame(meta, jpeg(3), 20_000)
        self.assertEqual(self.store.count(), 0)
        self.assertEqual(list(self.store.staging_dir.iterdir()), [])
        self.assertEqual(list(self.store.recordings_dir.iterdir()), [])
        self.assertIn("simulated disk failure", self.store.disk_error)

    def test_sqlite_failure_rolls_back_new_recording_file(self):
        meta = metadata("d" * 32, "motion_d")
        original_connect = self.store._connect

        class BrokenConnection:
            def __init__(self, delegate):
                self.delegate = delegate
            def __enter__(self):
                return self
            def __exit__(self, kind, value, traceback):
                if kind:
                    self.delegate.rollback()
                self.delegate.close()
                return False
            def execute(self, sql, *args):
                if sql.lstrip().upper().startswith("INSERT INTO FRAMES"):
                    raise receiver.sqlite3.OperationalError("simulated index failure")
                return self.delegate.execute(sql, *args)
            def commit(self):
                return self.delegate.commit()

        with mock.patch.object(self.store, "_connect", side_effect=lambda: BrokenConnection(original_connect())):
            with self.assertRaises(OSError):
                self.store.add_frame(meta, jpeg(4), 30_000)
        self.assertEqual(self.store.count(), 0)
        self.assertFalse((self.store.recordings_dir / "motion_d" / f"000_{'d'*32}.jpg").exists())

    def test_retention_removes_oldest_groups_until_under_byte_cap(self):
        first = metadata("1" * 32, "group_old", sequence=0)
        second = metadata("2" * 32, "group_new", sequence=0)
        self.store.add_frame(first, jpeg(1), 1000)
        self.store.add_frame(second, jpeg(2), 2000)
        # With the 1 KiB limit both remain; tighten the live limit and prune.
        self.store.max_bytes = 150
        self.assertEqual(self.store.apply_retention(3000), 1)
        self.assertEqual(self.store.count(), 1)
        self.assertIsNone(self.store.frame_path(first.capture_id))
        self.assertIsNotNone(self.store.frame_path(second.capture_id))

    def test_retention_removes_expired_group_as_a_unit(self):
        old = 1_000
        for seq, capture_id in enumerate(("5" * 32, "6" * 32)):
            self.store.add_frame(metadata(capture_id, "expired_group", sequence=seq), jpeg(seq), old + seq)
        cutoff = old + 2 + 31 * 24 * 60 * 60 * 1000
        self.assertEqual(self.store.apply_retention(cutoff), 2)
        self.assertEqual(self.store.count(), 0)

    def test_capture_time_uses_receiver_heartbeat_anchor(self):
        self.store.record_heartbeat({"boot_id": "boot1234", "monotonic_ms": 5000, "state": "armed"}, 50_000)
        self.store.add_frame(metadata("7" * 32, "motion_time", mono=2000), jpeg(5), 51_000)
        row = self.store.latest_frame()
        self.assertEqual(row["captured_ms"], 47_000)

    def test_heartbeat_disconnect_and_visible_disk_error(self):
        self.assertFalse(self.store.gallery_status(40_000)["connected"])
        self.store.record_heartbeat({"boot_id": "boot1234", "monotonic_ms": 1, "state": "armed"}, 40_000)
        self.assertTrue(self.store.gallery_status(55_000)["connected"])
        self.assertFalse(self.store.gallery_status(55_001)["connected"])
        self.store.disk_error = "disk full"
        self.assertFalse(self.store.gallery_status(55_001)["storage_ok"])
        self.assertEqual(self.store.gallery_status(55_001)["storage_error"], "disk full")
        with self.assertRaises(OSError):
            self.store.add_frame(metadata("8" * 32, "motion_error"), jpeg(6), 60_000)

    def test_queue_state_is_reported_as_capture_not_disk_confirmation(self):
        self.store.record_heartbeat({
            "boot_id": "boot1234", "monotonic_ms": 1, "state": "armed",
            "queued_images": 2, "queued_bytes": 200, "lost_images": 1,
            "status": "Camera capture failed",
        }, 40_000)
        status = self.store.gallery_status(41_000)
        self.assertEqual(status["capture_state"], "armed")
        self.assertEqual(status["queued_images"], 2)
        self.assertEqual(status["lost_images"], 1)
        self.assertEqual(status["device_status"], "Camera capture failed")
        self.assertIsNone(status["last_confirmed_save"])

    def test_scan_running_and_failure_are_persisted_as_explicit_incomplete_record(self):
        group = "scan_group_123"
        common = {"boot_id": "boot1234", "monotonic_ms": 1, "state": "scanning", "scan_group_id": group}
        self.store.record_heartbeat({**common, "scan_status": "scanning", "scan_message": ""}, 40_000)
        self.store.record_heartbeat({**common, "scan_status": "failed", "scan_message": "movement stalled"}, 45_000)
        self.assertEqual(self.store.recent_scans(), [{
            "group_id": group, "status": "failed", "started_ms": 40_000,
            "updated_ms": 45_000, "message": "movement stalled",
        }])


class ProtocolValidationTests(unittest.TestCase):
    def test_metadata_ranges_and_path_traversal_rejected(self):
        good = receiver.parse_frame_metadata("a" * 32, {
            "group_id": "scan_group", "kind": "scan", "view": "left",
            "sequence": "1", "boot_id": "boot1234", "monotonic_ms": "1234",
        })
        self.assertEqual(good.view, "left")
        for bad_id, fields in (
            ("../" + "a" * 29, {"group_id": "x", "kind": "motion", "view": "fixed", "sequence": "0", "boot_id": "boot1234", "monotonic_ms": "1"}),
            ("b" * 32, {"group_id": "../../etc", "kind": "motion", "view": "fixed", "sequence": "0", "boot_id": "boot1234", "monotonic_ms": "1"}),
            ("c" * 32, {"group_id": "x", "kind": "other", "view": "fixed", "sequence": "0", "boot_id": "boot1234", "monotonic_ms": "1"}),
        ):
            with self.assertRaises(ValueError):
                receiver.parse_frame_metadata(bad_id, fields)

    def test_gallery_is_local_site_and_uses_stockholm_display_zone(self):
        source = receiver.gallery_html()
        self.assertIn("timeZone:'Europe/Stockholm'", source)
        self.assertIn("/image/", source)
        self.assertIn("/scans", source)
        self.assertEqual(receiver.stockholm_time(0).endswith("CET"), True)


if __name__ == "__main__":
    unittest.main()
