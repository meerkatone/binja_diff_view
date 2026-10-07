"""Compare equal and unrelated selected-function block diffs.

Run each library in a fresh process:
    python3 tests/benchmark_block_pruning.py LIBRARY
"""

import ctypes
import hashlib
import json
import statistics
import sys
import time

library = ctypes.CDLL(sys.argv[1])
library.rust_diff_block_diff_json.argtypes = [ctypes.c_char_p]
library.rust_diff_block_diff_json.restype = ctypes.c_void_p
library.rust_diff_free_string.argtypes = [ctypes.c_void_p]
library.rust_diff_free_string.restype = None


def request(count, equal):
    def blocks(side):
        return [
            {
                "index": i,
                "lines": [{"text": f"line_{i}_{j}_{0 if equal else side}"} for j in range(30)],
                "successors": [],
            }
            for i in range(count)
        ]

    return json.dumps({
        "a": {"level": "HLIL", "entry": 0, "blocks": blocks(0)},
        "b": {"level": "HLIL", "entry": 0, "blocks": blocks(1)},
        "threshold": 0.5,
    }).encode()


for label, equal in [("equal", True), ("unrelated", False)]:
    payload = request(100, equal)
    times = []
    digest = None
    for _ in range(3):
        start = time.perf_counter()
        pointer = library.rust_diff_block_diff_json(payload)
        if not pointer:
            raise RuntimeError("engine returned null")
        try:
            result = json.loads(ctypes.string_at(pointer))
        finally:
            library.rust_diff_free_string(pointer)
        result_digest = hashlib.sha256(json.dumps(result, sort_keys=True).encode()).hexdigest()
        if digest is not None:
            assert digest == result_digest
        digest = result_digest
        times.append(time.perf_counter() - start)
    print(json.dumps({
        "case": label,
        "result_sha256": digest,
        "seconds": times,
        "median_seconds": statistics.median(times),
    }))
