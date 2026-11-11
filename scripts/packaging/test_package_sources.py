#!/usr/bin/env python3
"""The source-link check retries transient network failures instead of crashing."""

import http.client
import sys
import unittest
import urllib.error
from pathlib import Path
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import package_sources
from package_sources import HTTP_OK, URL_ATTEMPTS, reachable

URL = "https://launchpad.net/ubuntu/+source/example"
NOT_FOUND = 404
BAD_GATEWAY = 502


class Response:
    status = HTTP_OK

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        return False


def http_error(code):
    return urllib.error.HTTPError(URL, code, "error", {}, None)


class ReachableTests(unittest.TestCase):
    def setUp(self):
        sleep = mock.patch.object(package_sources.time, "sleep")
        sleep.start()
        self.addCleanup(sleep.stop)

    def check(self, *outcomes):
        with mock.patch.object(package_sources.urllib.request, "urlopen", side_effect=outcomes) as urlopen:
            result = reachable(URL)
        return result, urlopen.call_count

    def test_dropped_connection_is_retried(self):
        dropped = http.client.RemoteDisconnected("Remote end closed connection without response")
        self.assertEqual(self.check(dropped, Response()), (True, 2))

    def test_reset_and_timeout_are_retried(self):
        reset = ConnectionResetError("reset")
        self.assertEqual(self.check(reset, TimeoutError(), Response()), (True, 3))

    def test_server_error_is_retried(self):
        self.assertEqual(self.check(http_error(BAD_GATEWAY), Response()), (True, 2))

    def test_client_error_fails_at_once(self):
        self.assertEqual(self.check(http_error(NOT_FOUND)), (False, 1))

    def test_persistent_failure_gives_up(self):
        dropped = [http.client.RemoteDisconnected("closed")] * URL_ATTEMPTS
        self.assertEqual(self.check(*dropped), (False, URL_ATTEMPTS))


if __name__ == "__main__":
    unittest.main()
