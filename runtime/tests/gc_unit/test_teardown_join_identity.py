"""Offline record controls; these do not qualify an executed product arm."""
from contextlib import redirect_stdout
import io
import struct
import unittest

from check_teardown_records import verify_join_identity


def observed_records(machine='x86_64', clone3=False, expected=1):
    # A pthread handle, a kernel TID and a futex value are distinct identities.
    pid, tid, handle, output = 1700, 1749, 0x701000, 0x7ff010
    parent_tid, child_tid = 0x701100, 0x701108
    arm = machine == 'aarch64'
    flags = 0x3d0f00
    clone_args = [flags, 0x801000, parent_tid, 0x901000 if arm else child_tid,
                  child_tid if arm else 0x901000, 0]
    raw = None
    clone_nr = 220 if arm else 56
    if clone3:
        clone_nr = 435
        clone_args = [0x7ff100, 88, 0, 0, 0, 0]
        raw = struct.pack('<8Q', flags, 0, child_tid, parent_tid, 0, 0x801000, 4096, 0x901000).hex()
    clone = dict(caller=pid, syscall=clone_nr, args=clone_args, flags=flags,
                 parent_tid=parent_tid, child_tid=child_tid, clone3_raw=raw)
    binding = dict(clone, tid=tid, handle=handle, output=output,
                   parent_value=tid, child_value=expected)
    futex_nr = 98 if arm else 202
    wait_args = [child_tid, 265, expected, 0, 0, 0xffffffff]
    return {'tgid': pid, 'held': tid}, {
        'CONSTRUCTION_DOMAIN': [dict(machine=machine)],
        'PTHREAD_CREATE_ENTRY': [dict(caller=pid, output=output)],
        'CLONE_ABI': [clone],
        'CLONE': [dict(parent=pid, child=tid)],
        'PTHREAD_CLONE': [binding],
        'PTHREAD_JOIN_ENTRY': [dict(caller=pid, handle=handle, target=tid)],
        'JOIN_ABI': [dict(caller=pid, tid=tid, handle=handle, parent_tid=parent_tid,
                          child_tid=child_tid, uaddr=child_tid, value=expected,
                          args=wait_args, syscall=futex_nr)],
        'SYSCALL_ENTRY': [dict(tid=pid, nr=clone_nr, args=clone_args, length=80),
                          dict(tid=pid, nr=futex_nr, args=wait_args, length=80)],
        'JOIN_CLEARED': [dict(tid=tid, handle=handle, child_tid=child_tid, value=0)]}


class JoinIdentityRecords(unittest.TestCase):
    def check(self, records, ws, reason=None):
        output = io.StringIO()
        with redirect_stdout(output):
            if reason is None:
                verify_join_identity(lambda kind: records.get(kind, []), ws, 'exited')
            else:
                with self.assertRaisesRegex(ValueError, reason):
                    verify_join_identity(lambda kind: records.get(kind, []), ws, 'exited')
        self.assertIn('ASSERT_TEARDOWN_JOIN_IDENTITY phase=exited ' +
                      ('PASS' if reason is None else 'FAIL reason=' + reason), output.getvalue())

    def test_observed_value_is_not_a_tid(self):
        for machine in ('x86_64', 'aarch64'):
            for clone3 in (False, True):
                for expected in (1, 1749):
                    with self.subTest(machine=machine, clone3=clone3, expected=expected):
                        ws, records = observed_records(machine, clone3, expected)
                        self.check(records, ws)

    def test_wrong_public_join_target_rejected_at_identity_assertion(self):
        ws, records = observed_records()
        records['JOIN_ABI'][0]['handle'] += 8
        self.check(records, ws, 'join-target-handle')

    def test_unrelated_expected_one_wait_rejected(self):
        ws, records = observed_records()
        records['JOIN_ABI'][0]['uaddr'] += 8
        self.check(records, ws, 'wait-clear-address')

    def test_missing_public_join_is_not_construction_success(self):
        ws, records = observed_records()
        records['PTHREAD_JOIN_ENTRY'] = []
        self.check(records, ws, 'public-join-call')

    def test_wrong_create_output_rejected(self):
        ws, records = observed_records()
        records['PTHREAD_CREATE_ENTRY'][0]['output'] += 8
        self.check(records, ws, 'public-create-output')

    def test_clone3_raw_address_must_match_decoded_address(self):
        ws, records = observed_records(clone3=True)
        raw = bytearray.fromhex(records['CLONE_ABI'][0]['clone3_raw'])
        struct.pack_into('<Q', raw, 16, 0x701110)
        records['CLONE_ABI'][0]['clone3_raw'] = raw.hex()
        records['PTHREAD_CLONE'][0]['clone3_raw'] = raw.hex()
        self.check(records, ws, 'kernel-clear-contract')

    def test_raw_syscall_evidence_is_required(self):
        ws, records = observed_records()
        records['SYSCALL_ENTRY'] = [records['SYSCALL_ENTRY'][1]]
        self.check(records, ws, 'raw-clone-entry')

    def test_nonreap_clear_must_be_same_address_and_zero(self):
        for field, value in (('child_tid', 0x701110), ('value', 1), ('tid', 1700)):
            ws, records = observed_records()
            records['JOIN_CLEARED'][0][field] = value
            self.check(records, ws, 'nonreap-kernel-clear')


if __name__ == '__main__':
    unittest.main()
