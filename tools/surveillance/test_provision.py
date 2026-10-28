import importlib.util
import tempfile
import unittest
from pathlib import Path

MODULE_PATH = Path(__file__).with_name("provision.py")
SPEC = importlib.util.spec_from_file_location("surveillance_provision", MODULE_PATH)
provision = importlib.util.module_from_spec(SPEC)
assert SPEC.loader is not None
SPEC.loader.exec_module(provision)


class ProvisioningTests(unittest.TestCase):
    def test_profile_escapes_certificate_newlines_as_cpp_string_data(self):
        certificate = "-----BEGIN CERTIFICATE-----\nabc123\n-----END CERTIFICATE-----\n"
        with tempfile.TemporaryDirectory() as directory:
            profile = Path(directory) / "connection_profile.h"
            provision.write_profile(profile, "pairing_token", certificate, "receiver.local", 8443)
            contents = profile.read_text(encoding="utf-8")
        self.assertIn('static constexpr const char kSurveillanceCaPem[] = "-----BEGIN CERTIFICATE-----\\nabc123\\n-----END CERTIFICATE-----\\n";', contents)
        self.assertNotIn("BEGIN CERTIFICATE-----\nabc123", contents)


if __name__ == "__main__":
    unittest.main()
