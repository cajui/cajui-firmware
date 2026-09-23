import importlib.util
import json
import os
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

spec = importlib.util.spec_from_file_location("provision", Path(__file__).resolve().parents[1] / "tools/provision.py")
provision = importlib.util.module_from_spec(spec)
spec.loader.exec_module(provision)


class Device:
    def __init__(self, role, identity, events):
        self.role, self.identity, self.events = role, identity, events
        self.network, self.receiver, self.profile = "0" * 16, "0" * 16, "0000"
        self.enrollment = None
        self.state, self.counter = 0, 0
        self.fail = None
        self.boot = 1

    def request(self, command):
        words = command.split()
        verb = words[0]
        self.events.append((self.role, verb))
        if self.fail == verb:
            self.fail = None
            raise provision.ProvisioningError("Simulated disconnect")
        if verb == "HELLO":
            return [self.identity, self.role, "ready", self.network, self.receiver, self.profile, "0", f"{self.boot:08x}"]
        if words[1] != self.identity:
            raise provision.ProvisioningError("Wrong device identity")
        if verb == "PREPARE":
            proposed = words[2:]
            if self.enrollment == proposed:
                return []
            if self.enrollment is not None:
                raise provision.ProvisioningError("Different transaction")
            self.enrollment = proposed
            self.network, self.receiver, self.profile = words[2], words[3], words[7]
            self.state = 1
        elif verb == "INFO":
            if self.enrollment is None:
                raise provision.ProvisioningError("Device rejected request: NOT_FOUND")
            return [str(self.state), f"{self.counter:016x}"]
        elif verb == "ACTIVATE":
            self.state = 2
        elif verb == "RESERVE":
            self.counter += 1
            return [f"{self.counter:016x}"]
        elif verb == "REBOOT":
            self.boot += 1  # Durable state survives; C++ storage has its own tests.
        else:
            raise provision.ProvisioningError("Unknown command")
        return []


class ProvisionTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.path = Path(self.temporary.name) / "recovery.json"
        self.events = []
        self.tx = Device("tx", "0000000000000002", self.events)
        self.rx = Device("rx", "0000000000000001", self.events)

    def tearDown(self):
        self.temporary.cleanup()

    def test_enrollment_orders_steps_and_hides_credentials(self):
        output = provision.enroll(self.tx, self.rx, self.path)
        self.assertTrue(output["configured"])
        self.assertFalse(output["radio_validated"])
        transaction = provision.load(self.path)
        self.assertEqual(0o600, self.path.stat().st_mode & 0o777)
        self.assertNotIn(transaction["key"], json.dumps(output))
        mutations = [item for item in self.events if item[1] in {"PREPARE", "ACTIVATE"}]
        self.assertEqual([("rx", "PREPARE"), ("tx", "PREPARE"), ("rx", "ACTIVATE"), ("tx", "ACTIVATE")], mutations)
        self.assertEqual("configured", transaction["phase"])

    def test_each_interruption_can_resume_without_resetting_counter(self):
        for role, verb in (("rx", "PREPARE"), ("tx", "PREPARE"), ("rx", "ACTIVATE"), ("tx", "ACTIVATE")):
            with self.subTest(role=role, verb=verb), tempfile.TemporaryDirectory() as folder:
                tx, rx = Device("tx", self.tx.identity, []), Device("rx", self.rx.identity, [])
                path = Path(folder) / "recovery.json"
                (tx if role == "tx" else rx).fail = verb
                with self.assertRaises(provision.ProvisioningError):
                    provision.enroll(tx, rx, path)
                initial = provision.load(path)
                provision.enroll(tx, rx, path, resume=True)
                self.assertEqual(initial["key"], provision.load(path)["key"])
                tx.counter = 17
                provision.enroll(tx, rx, path, resume=True)
                self.assertEqual(17, tx.counter)

    def test_recovery_file_exists_before_first_device_write(self):
        original = self.rx.request
        def inspect(command):
            if command.startswith("PREPARE"):
                self.assertEqual("new", provision.load(self.path)["phase"])
            return original(command)
        self.rx.request = inspect
        provision.enroll(self.tx, self.rx, self.path)

    def test_wrong_ports_or_network_are_rejected_before_mutation(self):
        with self.assertRaises(provision.ProvisioningError):
            provision.enroll(self.rx, self.tx, self.path)
        self.assertFalse(self.path.exists())
        self.tx.network = "000000000000ffff"
        with self.assertRaises(provision.ProvisioningError):
            provision.enroll(self.tx, self.rx, self.path)
        self.assertIsNone(self.rx.enrollment)

    def test_resume_rejects_erased_previously_configured_device(self):
        provision.enroll(self.tx, self.rx, self.path)
        self.tx.enrollment = None
        self.tx.network, self.tx.receiver, self.tx.profile = "0" * 16, "0" * 16, "0000"
        with self.assertRaises(provision.ProvisioningError):
            provision.enroll(self.tx, self.rx, self.path, resume=True)
        self.assertIsNone(self.tx.enrollment)

    def test_file_permissions_schema_and_symlink(self):
        provision.enroll(self.tx, self.rx, self.path)
        os.chmod(self.path, 0o644)
        with self.assertRaises(provision.ProvisioningError):
            provision.load(self.path)
        os.chmod(self.path, 0o600)
        link = self.path.with_name("link.json")
        link.symlink_to(self.path)
        with self.assertRaises(OSError):
            provision.load(link)
        transaction = provision.load(self.path)
        transaction["unexpected"] = True
        provision.save(self.path, transaction)
        with self.assertRaises(provision.ProvisioningError):
            provision.load(self.path)

    def test_create_never_overwrites_existing_recovery(self):
        provision.enroll(self.tx, self.rx, self.path)
        before = self.path.read_bytes()
        with self.assertRaises(FileExistsError):
            provision.enroll(self.tx, self.rx, self.path)
        self.assertEqual(before, self.path.read_bytes())

    def test_verify_restart_burns_counters_but_does_not_claim_radio(self):
        provision.enroll(self.tx, self.rx, self.path)
        result = provision.verify_restart(self.tx, self.rx, self.path)
        self.assertTrue(result["restart_verified"])
        self.assertEqual(1, result["counter_before"])
        self.assertEqual(2, result["counter_after"])
        self.assertFalse(result["radio_validated"])

    def test_counter_reset_on_reboot_fails_verification(self):
        provision.enroll(self.tx, self.rx, self.path)
        original = self.tx.request
        def resetting(command):
            if command.startswith("REBOOT"):
                self.tx.counter = 0
            return original(command)
        self.tx.request = resetting
        with self.assertRaises(provision.ProvisioningError):
            provision.verify_restart(self.tx, self.rx, self.path)

    def test_malformed_identifiers_and_recovery_file(self):
        for value in (None, 123, "abc", "g" * 16, "F" * 16):
            with self.assertRaises(provision.ProvisioningError):
                provision.identifier(value)
        provision.enroll(self.tx, self.rx, self.path)
        transaction = provision.load(self.path)
        for field, bad in (("version", 9), ("generation", "0" * 16), ("key", "0" * 32),
                           ("profile", "0002"), ("phase", "invalid")):
            changed = dict(transaction); changed[field] = bad
            provision.save(self.path, changed)
            with self.assertRaises(provision.ProvisioningError):
                provision.load(self.path)


class FakeSerial:
    def __init__(self, lines):
        self.lines = list(lines)
        self.sent = []
    def reset_input_buffer(self): pass
    def write(self, data): self.sent.append(data)
    def flush(self): pass
    def readline(self, limit): return self.lines.pop(0) if self.lines else b""


class TransportTests(unittest.TestCase):
    def link(self, lines):
        link = provision.SerialLink.__new__(provision.SerialLink)
        link.serial = FakeSerial(lines)
        return link

    def test_boot_noise_is_ignored_and_command_is_framed(self):
        link = self.link([b"boot output\n", b"CJ1 OK ACTIVATE\n"])
        self.assertEqual([], link.request("ACTIVATE device node generation"))
        self.assertEqual(b"CJ1 ACTIVATE device node generation\n", link.serial.sent[0])

    def test_device_errors_never_echo_a_secret(self):
        secret = "0123456789abcdef0123456789abcdef"
        link = self.link([("CJ1 ERR " + secret + "\n").encode()])
        with self.assertRaises(provision.ProvisioningError) as caught:
            link.request("PREPARE " + secret)
        self.assertNotIn(secret, str(caught.exception))

    def test_timeout_and_wrong_response_type(self):
        link = self.link([])
        with patch.object(provision.time, "monotonic", side_effect=[0, 1, 6]):
            with self.assertRaises(provision.ProvisioningError):
                link.request("HELLO")
        with self.assertRaises(provision.ProvisioningError):
            self.link([b"CJ1 OK PREPARE\n"]).request("HELLO")

    def test_malformed_hello_is_rejected(self):
        class Reply:
            def request(self, command):
                return ["0" * 16, "tx", "ready", "0" * 16, "0" * 16, "0001", "invalid", "00000001"]
        with self.assertRaises(provision.ProvisioningError):
            provision.hello(Reply())

    def test_storage_failure_reason_is_reported(self):
        class Reply:
            def __init__(self, health):
                self.health = health
            def request(self, command):
                return ["0" * 16, "rx", self.health, "0" * 16, "0" * 16, "0001", "0", "00000001"]
        for health, reason in (("role", "role"), ("bogus", "unknown")):
            with self.assertRaises(provision.ProvisioningError) as caught:
                provision.hello(Reply(health))
            self.assertEqual("Device storage is unavailable: " + reason, str(caught.exception))


if __name__ == "__main__":
    unittest.main()
