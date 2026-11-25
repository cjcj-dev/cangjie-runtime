"""Replay Linux wait statuses against the teardown probe's ownership checks."""
import os
import signal
import threading
import unittest
from unittest.mock import call, patch

import check_teardown_exit as probe


def stop(sig, event=0):
    return (event << 16) | (sig << 8) | 0x7f


class CloneStops(unittest.TestCase):
    def setUp(self):
        self.pid = os.getpid()
        self.tracees = probe.Tracees(self.pid)
        # Use a live non-leader TID so the probe checks real procfs membership.
        done = threading.Event()
        thread = threading.Thread(target=done.wait)
        thread.start()
        self.addCleanup(thread.join)
        self.addCleanup(done.set)
        self.child = thread.native_id
        self.initial = (self.child, stop(signal.SIGSTOP))
        self.clone = (self.pid, stop(signal.SIGTRAP, 3))

    def test_child_stop_before_clone(self):
        words = {}

        def resumed(request, tid):
            self.assertEqual((request, tid), (probe.CONT, self.child))
            self.assertIn(tid, self.tracees.owned)
            self.assertEqual(words[tid], 0x5000)

        with patch.object(probe.os, 'waitpid', side_effect=[self.initial, self.clone]), \
                patch.object(probe, 'trace', side_effect=resumed) as trace:
            self.assertEqual(self.tracees.wait(collecting_clones=True), self.clone)
            self.assertEqual(self.tracees.early_clone_stops, {self.child})
            self.assertEqual(self.tracees.owned, {self.pid})
            trace.assert_not_called()
            self.tracees.confirm_clone(self.child, words, 0x5000)
            trace.assert_called_once_with(probe.CONT, self.child)
            self.tracees.require_paired()

    def test_parent_event_before_child_stop(self):
        with patch.object(probe, 'trace') as trace:
            self.tracees.confirm_clone(self.child, {}, 0x5000)
            with patch.object(probe.os, 'waitpid', return_value=self.initial):
                self.assertEqual(self.tracees.wait(collecting_clones=True), self.initial)
            trace.assert_not_called()  # The caller dispatches this stop normally.

    def reject_wait(self, who, status):
        with patch.object(probe.os, 'waitpid', side_effect=[(who, status)]), \
                self.assertRaisesRegex(RuntimeError, f'^unowned wait tid={who}$'):
            self.tracees.wait(collecting_clones=True)
        self.assertEqual(self.tracees.owned, {self.pid})

    def test_unrelated_process(self):
        self.reject_wait(os.getppid(), stop(signal.SIGSTOP))

    def test_unknown_status(self):
        for status in (stop(signal.SIGTRAP), stop(signal.SIGSTOP, 3), 0, signal.SIGKILL):
            with self.subTest(status=status):
                self.reject_wait(self.child, status)

    def test_duplicate_initial_stop(self):
        self.tracees.early_clone_stops.add(self.child)
        self.reject_wait(*self.initial)

    def test_unpaired_at_completion(self):
        self.tracees.early_clone_stops.add(self.child)
        with self.assertRaisesRegex(RuntimeError, 'unpaired early clone stops'):
            self.tracees.require_paired()
        with patch.object(probe.os, 'waitpid', return_value=(self.pid, 0)), \
                self.assertRaisesRegex(RuntimeError, 'unpaired early clone stops'):
            self.tracees.wait()

    def test_binding_failure_does_not_resume_child(self):
        self.tracees.early_clone_stops.add(self.child)
        with patch.object(probe, 'trace') as trace:
            with self.assertRaisesRegex(RuntimeError, 'duplicate clone clear word'):
                self.tracees.confirm_clone(self.child, {self.pid: 0x5000}, 0x5000)
            trace.assert_not_called()
        self.assertEqual(self.tracees.early_clone_stops, {self.child})
        self.assertEqual(self.tracees.owned, {self.pid})

    def test_cleanup_reaps_unpaired_child(self):
        self.tracees.early_clone_stops.add(self.child)
        events = [(self.child, stop(signal.SIGTRAP, 6)),
                  (self.child, signal.SIGKILL), (self.pid, signal.SIGKILL)]
        with patch.object(probe.os, 'kill') as kill, \
                patch.object(probe.os, 'waitpid', side_effect=events) as wait, \
                patch.object(probe, 'trace') as trace:
            self.tracees.kill_and_reap()
            kill.assert_called_once_with(self.pid, signal.SIGKILL)
            trace.assert_called_once_with(probe.CONT, self.child, data=signal.SIGKILL)
            self.assertEqual(wait.call_args_list,
                             [call(self.child, probe.WALL), call(self.child, probe.WALL), call(-1, probe.WALL)])
        self.assertEqual(self.tracees.reaped, {self.pid, self.child})
        self.assertEqual(self.tracees.owned, {self.pid})


if __name__ == '__main__':
    unittest.main()
