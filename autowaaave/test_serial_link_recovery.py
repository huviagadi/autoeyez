"""Exercise the bridge's short reopen path after a USB write timeout."""
import time
import unittest

from serial_link import SerialLink


class FakePort:
    def __init__(self, fail_progress=False):
        self.fail_progress = fail_progress
        self.in_waiting = 0
        self.flushed = False

    def write(self, data):
        if self.fail_progress and data == b"PROGRESS:1\n":
            raise TimeoutError("Write timeout")
        return len(data)

    def read(self, size):
        time.sleep(.005)
        return b""

    def close(self):
        if self.fail_progress and not self.flushed:
            raise AssertionError("stalled output must be discarded before close")

    def reset_output_buffer(self):
        self.flushed = True


class SerialRecoveryTest(unittest.TestCase):
    def test_live_port_reopens_promptly_after_write_timeout(self):
        opens = []

        def opener(path):
            opens.append(time.monotonic())
            return FakePort(fail_progress=len(opens) == 1)

        link = SerialLink(lambda line: None, opener=opener,
                          resolver=lambda: "fake", retry=1.0)
        try:
            deadline = time.monotonic() + 1
            while not link.connected and time.monotonic() < deadline:
                time.sleep(.005)
            self.assertTrue(link.connected)
            link.send(b"PROGRESS:1\n")
            while len(opens) < 2 and time.monotonic() < deadline:
                time.sleep(.005)
            self.assertGreaterEqual(len(opens), 2)
            self.assertLess(opens[1] - opens[0], .5)
            self.assertEqual(link.generation, 2)
        finally:
            link.close()


if __name__ == "__main__":
    unittest.main()
