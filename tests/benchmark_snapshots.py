"""Compare legacy JSON and prepared-snapshot paths on a synthetic 10-build corpus.

Usage: python3 tests/benchmark_snapshots.py BEFORE_LIBRARY AFTER_LIBRARY
No Binary Ninja session, database, or persistent plugin cache is used.
"""
import ctypes
import hashlib
import itertools
import json
from pathlib import Path
import statistics
import sys
import time


class Engine:
    def __init__(self, path, prepared=False):
        self.path = Path(path).resolve()
        self.lib = ctypes.CDLL(str(self.path))
        self.lib.rust_diff_diff_json.argtypes = [ctypes.c_char_p, ctypes.c_char_p]
        self.lib.rust_diff_diff_json.restype = ctypes.c_void_p
        self.lib.rust_diff_free_string.argtypes = [ctypes.c_void_p]
        self.lib.rust_diff_free_string.restype = None
        if prepared:
            for name, args, result in [
                ("rust_diff_prepare_json_v1", [ctypes.c_char_p, ctypes.c_void_p, ctypes.c_void_p], ctypes.c_void_p),
                ("rust_diff_free_prepared_v1", [ctypes.c_void_p], None),
                ("rust_diff_prepared_size_v1", [ctypes.c_void_p], ctypes.c_size_t),
                ("rust_diff_diff_prepared_v1", [ctypes.c_void_p] * 4, ctypes.c_void_p),
            ]:
                fn = getattr(self.lib, name)
                fn.argtypes, fn.restype = args, result

    def result(self, pointer):
        if not pointer:
            raise RuntimeError("engine returned null")
        try:
            value = json.loads(ctypes.string_at(pointer))
            value.pop("analysis_time", None)
            return value
        finally:
            self.lib.rust_diff_free_string(pointer)

    def json_diff(self, a, b):
        return self.result(self.lib.rust_diff_diff_json(a, b))

    def prepare(self, value):
        handle = self.lib.rust_diff_prepare_json_v1(value, None, None)
        if not handle:
            raise RuntimeError("preparation failed")
        return handle

    def diff(self, a, b):
        return self.result(self.lib.rust_diff_diff_prepared_v1(a, b, None, None))


def snapshot(build, count=200):
    functions = []
    for index in range(count):
        address = 0x10000 + build * 0x100000 + index * 0x100
        instructions = [
            {"address": address + ins * 4, "mnemonic": "add" if ins % 3 == 0 else "mov",
             "operands": ["r0", str(index * 100 + ins + (build if index % 7 == 0 and ins == 12 else 0))], "length": 4}
            for ins in range(24)
        ]
        functions.append({"name": f"unit_{index}", "address": address, "size": 96,
                          "basic_blocks": [{"address": address, "size": 96, "instructions": instructions}],
                          "strings": [f"sample {index}"]})
    # Exercise the unsorted-input ownership path and original unmatched order too.
    if build % 2:
        functions.reverse()
    return json.dumps(functions, separators=(",", ":")).encode()


def main():
    before, after = Engine(sys.argv[1]), Engine(sys.argv[2], prepared=True)
    inputs = [snapshot(build) for build in range(10)]
    pairs = list(itertools.combinations(range(len(inputs)), 2))
    handles = []
    try:
        for value in inputs:
            handles.append(after.prepare(value))
        for a, b in pairs:
            expected = before.json_diff(inputs[a], inputs[b])
            assert after.diff(handles[a], handles[b]) == expected, (a, b, "prepared mismatch")
            assert after.json_diff(inputs[a], inputs[b]) == expected, (a, b, "JSON mismatch")
        empty = after.prepare(b"[]")
        try:
            assert after.diff(handles[1], empty) == before.json_diff(inputs[1], b"[]"), "unmatched metadata/order"
        finally:
            after.lib.rust_diff_free_prepared_v1(empty)
        estimated_bytes = sum(after.lib.rust_diff_prepared_size_v1(handle) for handle in handles)
    finally:
        for handle in handles:
            after.lib.rust_diff_free_prepared_v1(handle)

    timings = {"before_json": [], "after_json": [], "after_prepared_including_preparation": []}
    for _ in range(3):
        for label, engine in [("before_json", before), ("after_json", after)]:
            start = time.perf_counter()
            for a, b in pairs:
                engine.json_diff(inputs[a], inputs[b])
            timings[label].append(time.perf_counter() - start)
        handles = []
        start = time.perf_counter()
        try:
            for value in inputs:
                handles.append(after.prepare(value))
            for a, b in pairs:
                after.diff(handles[a], handles[b])
        finally:
            for handle in handles:
                after.lib.rust_diff_free_prepared_v1(handle)
        timings["after_prepared_including_preparation"].append(time.perf_counter() - start)

    print(json.dumps({"builds": 10, "functions_per_build": 200, "instructions_per_function": 24,
        "comparisons": len(pairs), "result_parity": True, "prepared_estimated_bytes": estimated_bytes,
        "seconds": timings, "median_seconds": {key: statistics.median(value) for key, value in timings.items()},
        "before_sha256": hashlib.sha256(before.path.read_bytes()).hexdigest(),
        "after_sha256": hashlib.sha256(after.path.read_bytes()).hexdigest()}, indent=2))


if __name__ == "__main__":
    main()
