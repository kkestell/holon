"""Run Holon's subprocess checks; optionally compile with sanitizers."""

import os
from pathlib import Path
import select
import shlex
import signal
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parent
SANITIZE = "--sanitize" in sys.argv
if SANITIZE:
    sys.argv.remove("--sanitize")


class HolonTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.build = tempfile.TemporaryDirectory(prefix="holon-build-")
        cls.binary = Path(cls.build.name) / "holon"
        flags = ["-std=c11", "-Wall", "-Wextra", "-Werror", "-pedantic", "-DHOLON_TESTING"]
        flags += ["-O1", "-g", "-fsanitize=address,undefined", "-fno-omit-frame-pointer"] if SANITIZE else ["-O2"]
        subprocess.run(
            [*shlex.split(os.environ.get("CC", "cc")), *flags,
             str(ROOT / "holon.c"), "-o", str(cls.binary)], check=True,
        )
        cls.env = dict(os.environ, ASAN_OPTIONS="detect_leaks=0", UBSAN_OPTIONS="halt_on_error=1")
        cls.env.pop("HOLON_STOP_AT", None)

    @classmethod
    def tearDownClass(cls):
        cls.build.cleanup()

    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="holon-test-")
        self.addCleanup(self.temp.cleanup)
        self.directory = Path(self.temp.name)
        self.store = self.directory / "world"

    def run_command(self, command, *args, error=None):
        result = subprocess.run(
            [str(self.binary), command, str(self.store), *map(str, args)],
            text=True, capture_output=True, env=self.env, timeout=20,
        )
        if error is None:
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(result.stderr, "")
        else:
            self.assertEqual(result.returncode, 1, result.stderr)
            self.assertIn(error, result.stderr)
        return result.stdout.strip()

    def init_text(self, text):
        program = self.directory / "program.holon"
        program.write_text(text)
        return self.run_command("init", program)

    def drain_values(self):
        return [line.split(" ", 2)[2] for line in self.run_command("drain").splitlines()]

    def interrupted(self, point, command, *args):
        process = subprocess.Popen(
            [str(self.binary), command, str(self.store), *map(str, args)],
            stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
            env=dict(self.env, HOLON_STOP_AT=point),
        )
        try:
            ready, _, _ = select.select([process.stderr], [], [], 20)
            self.assertTrue(ready, "checkpoint timeout")
            self.assertEqual(process.stderr.readline(), f"stopped: {point}\n")
        finally:
            process.kill()
            output, errors = process.communicate(timeout=10)
        self.assertEqual(process.returncode, -signal.SIGKILL)
        self.assertEqual(errors, "")
        return output.strip()

    def test_existing_counter_and_restarts(self):
        self.assertEqual(self.run_command("init", ROOT / "examples" / "counter.holon"), "0")
        self.assertEqual(self.run_command("send", "2"), "2")
        self.assertEqual(self.run_command("send", "3"), "5")
        self.assertEqual(self.run_command("inspect"), "5")
        before = self.store.read_bytes()
        self.run_command("init", ROOT / "examples" / "counter.holon", error="store already exists")
        self.run_command("send", '"bad"', error="TypeMismatch")
        self.run_command("send", "18446744073709551616", error="integer overflow")
        self.run_command("send", "[2 3]", error="parse error")
        self.assertEqual(self.store.read_bytes(), before)
        deliveries = self.run_command("drain").splitlines()
        self.assertRegex(deliveries[0], r"^effect: [0-9a-f]{32}:2:0 2$")
        self.assertEqual(deliveries[1], deliveries[0].replace(":2:0 2", ":3:0 5"))
        self.assertEqual(self.run_command("drain"), "")
        self.assertEqual(self.run_command("inspect"), "5")

    def test_upgrade_and_failed_migration(self):
        self.run_command("init", ROOT / "examples" / "counter.holon")
        self.run_command("send", "5")
        before = self.store.read_bytes()
        self.run_command("send", '[[quote, 0], [trap, [quote, "migration failed"]]]',
                         error='trap: "migration failed"')
        self.assertEqual(self.store.read_bytes(), before)
        self.assertEqual(self.run_command("send", (ROOT / "examples" / "upgrade.holon").read_text()), "[5, 0]")
        self.assertEqual(self.run_command("inspect"), "[5, 0]")
        self.assertEqual(self.run_command("send", "3"), "[8, 1]")
        self.assertEqual(self.run_command("inspect"), "[8, 1]")
        self.assertEqual(self.drain_values(), ["5", '"upgraded"', "[8, 1]"])
        self.assertEqual(self.run_command("drain"), "")

    def test_gcd_example(self):
        self.run_command("init", ROOT / "examples" / "gcd.holon")
        self.assertEqual(self.run_command("send", "[1071, 462]"), "21")
        self.assertEqual(self.run_command("send", "[0, 5]"), "5")
        self.assertEqual(self.run_command("send", "[7, 0]"), "7")
        self.assertEqual(self.drain_values(), ["21", "5", "7"])

    def test_fizzbuzz_example(self):
        self.run_command("init", ROOT / "examples" / "fizzbuzz.holon")
        self.assertEqual(self.run_command("send", "5"), "5")
        self.assertEqual(self.run_command("send", "10"), "15")
        self.assertEqual(self.drain_values(), [
            "1", "2", '"Fizz"', "4", '"Buzz"', '"Fizz"', "7", "8", '"Fizz"', '"Buzz"',
            "11", '"Fizz"', "13", "14", '"FizzBuzz"',
        ])

    def test_turnstile_example(self):
        self.assertEqual(self.run_command("init", ROOT / "examples" / "turnstile.holon"), '"locked"')
        for event, state in [("push", "locked"), ("coin", "unlocked"), ("coin", "unlocked"),
                             ("push", "locked")]:
            self.assertEqual(self.run_command("send", f'"{event}"'), f'"{state}"')
        before = self.store.read_bytes()
        self.run_command("send", '"kick"', error='trap: "unknown event"')
        self.assertEqual(self.store.read_bytes(), before)
        self.assertEqual(self.drain_values(), ['"unlock"', '"lock"'])

    def test_bank_example(self):
        self.run_command("init", ROOT / "examples" / "bank.holon")
        self.assertEqual(self.run_command("send", '["deposit", 100]'), "100")
        self.assertEqual(self.run_command("send", '["withdraw", 30]'), "70")
        before = self.store.read_bytes()
        self.run_command("send", '["withdraw", 71]', error='trap: "insufficient funds"')
        self.run_command("send", '["transfer", 1]', error='trap: "unknown operation"')
        self.assertEqual(self.store.read_bytes(), before)
        self.assertEqual(self.run_command("send", '["withdraw", 70]'), "0")
        self.assertEqual(self.drain_values(),
                         ['["deposit", 100, 100]', '["withdraw", 30, 70]', '["withdraw", 70, 0]'])

    def test_opcodes_and_alu(self):
        self.init_text("[[[tuple, [get, path[0]], [tuple, [eval, [quote, []], [get, path[1]]]]]], 0]")
        cases = [
            ('[quote, [x"00ff", "a\\\"b", [], "", "é"]]', '[x"00ff", "a\\\"b", [], "", x"c3a9"]'),
            ("[tuple, [quote, 2], [quote, 3]]", "[2, 3]"),
            ("[tuple, [kind, [quote, 0]], [kind, [quote, []]]]", "[0, 1]"),
            ('[len, [quote, "abc"]]', "3"),
            ("[pipe, [quote, [2, [3]]], [get, path[1, 0]]]", "3"),
            ("[pipe, [quote, [2, [3]]], [set, path[1, 0], [quote, 4]]]", "[2, [4]]"),
            ("[set, path[], [quote, 9]]", "9"),
            ("[get_at, [quote, [2, 3]], [quote, 1]]", "3"),
            ('[get_at, [quote, "ab"], [quote, 1]]', '"b"'),
            ("[set_at, [quote, [2, 3]], [quote, 1], [quote, 4]]", "[2, 4]"),
            ('[slice, [quote, "abc"], [quote, 1], [quote, 2]]', '"bc"'),
            ("[slice, [quote, [2, 3]], [quote, 2], [quote, 0]]", "[]"),
            ('[concat, [quote, "ab"], [quote, "cd"]]', '"abcd"'),
            ("[concat, [quote, [2]], [quote, [3]]]", "[2, 3]"),
            ("[if, [quote, true], [quote, 7], []]", "7"),
            ("[if, [quote, false], [], [quote, 8]]", "8"),
            ("[call, [quote, [[[get, path[1]]], 0]], [quote, 0], [quote, 9]]", "9"),
            ("[eq, [quote, [1, [2]]], [tuple, [quote, 1], [quote, [2]]]]", "1"),
            ("[eq, [quote, []], [quote, x\"\"]]", "0"),
        ]
        for operation, a, b, expected in [
            ("add", 2**64 - 1, 1, 0), ("sub", 0, 1, 2**64 - 1),
            ("mul", 2**63, 2, 0), ("div", 7, 2, 3), ("mod", 7, 2, 1),
            ("and", 6, 3, 2), ("or", 6, 3, 7), ("xor", 6, 3, 5),
            ("shl", 3, 2, 12), ("shr", 12, 2, 3), ("lt", 2, 3, 1),
            ("le", 3, 3, 1), ("shl", 1, 64, 0), ("shr", 1, 64, 0),
        ]:
            cases.append((f"[alu, {operation}, [quote, {a}], [quote, {b}]]", str(expected)))
        for expression, _ in cases:
            with self.subTest(expression=expression):
                self.run_command("send", expression)
        self.assertEqual(self.drain_values(), [v for _, v in cases])
        before = self.store.read_bytes()
        for expression, error in [
            ("[alu, div, [quote, 1], [quote, 0]]", "DivideByZero"),
            ("[if, [quote, 2], [quote, 0], [quote, 0]]", "TypeMismatch"),
            ("[get, []]", "MalformedAst"), ("[quote]", "MalformedAst"),
            ("[set, path[0], [trap, [quote, 99]]]", "IndexOutOfBounds"),
            ("[set_at, [quote, 0], [trap, [quote, 99]], [quote, 0]]", "TypeMismatch"),
            ("[alu, 12, [trap, [quote, 99]], [quote, 0]]", "MalformedAst"),
            ("[slice, [quote, []], [quote, 1], [quote, 0]]", "IndexOutOfBounds"),
            ("[concat, [quote, []], [quote, 0]]", "TypeMismatch"),
            ('[trap, [quote, "abort"]]', 'trap: "abort"'),
        ]:
            with self.subTest(expression=expression):
                self.run_command("send", expression, error=error)
                self.assertEqual(self.store.read_bytes(), before)

    def test_invalid_transition(self):
        self.init_text("[[[get, path[1]]], 0]")
        before = self.store.read_bytes()
        for event in ["0", "[0, []]", "[[[], 0], []]", "[[[[quote, 0]], 0], 0]"]:
            self.run_command("send", event, error="TypeMismatch")
            self.assertEqual(self.store.read_bytes(), before)

    def test_tail_calls(self):
        self.init_text("""[[
            [tuple, [get, path[0]], [tuple, [call, [get, path[0]], [quote, 1], [get, path[1]]]]],
            [if, [eq, [get, path[1]], [quote, 0]], [quote, 42],
                [call, [get, path[0]], [quote, 1], [alu, sub, [get, path[1]], [quote, 1]]]]
        ], 0]""")
        self.run_command("send", "30000")
        self.assertTrue(self.run_command("drain").endswith(":2:0 42"))

    def test_interrupted_snapshot_and_delivery(self):
        self.run_command("init", ROOT / "examples" / "counter.holon")
        before = self.store.read_bytes()
        self.interrupted("before-rename", "send", "2")
        self.assertEqual(self.store.read_bytes(), before)
        self.assertEqual(self.run_command("inspect"), "0")
        self.assertEqual(self.run_command("drain"), "")
        self.interrupted("after-rename", "send", "2")
        self.assertEqual(self.run_command("inspect"), "2")
        self.run_command("send", "3")
        before = self.store.read_bytes()
        first = self.interrupted("after-delivery", "drain")
        self.assertEqual(self.store.read_bytes(), before)
        replay = self.run_command("drain").splitlines()
        self.assertEqual(replay[0], first)
        self.assertTrue(replay[1].endswith(":3:0 5"))
        self.assertEqual(self.run_command("drain"), "")
        self.assertEqual(self.run_command("inspect"), "5")

    def test_failed_output_keeps_effect_pending(self):
        self.run_command("init", ROOT / "examples" / "counter.holon")
        self.run_command("send", "2")
        before = self.store.read_bytes()
        read_fd, write_fd = os.pipe()
        os.close(read_fd)
        try:
            result = subprocess.run([str(self.binary), "drain", str(self.store)],
                                    stdout=write_fd, stderr=subprocess.PIPE, env=self.env, timeout=20)
        finally:
            os.close(write_fd)
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(self.store.read_bytes(), before)
        self.assertTrue(self.run_command("drain").endswith(":2:0 2"))


if __name__ == "__main__":
    unittest.main(verbosity=2)
