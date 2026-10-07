"""Benchmark sparse call-graph refinement inside a duplicate-body bucket.

Run each library in a fresh process:
    python3 tests/benchmark_neighbor_refinement.py LIBRARY [FUNCTION_COUNT]
"""

import ctypes
import hashlib
import json
import statistics
import sys
import time

library = ctypes.CDLL(sys.argv[1])
library.rust_diff_diff_json.argtypes = [ctypes.c_char_p, ctypes.c_char_p]
library.rust_diff_diff_json.restype = ctypes.c_void_p
library.rust_diff_free_string.argtypes = [ctypes.c_void_p]


def payload(base, count):
    result = []
    for index in range(count):
        address = base + index * 32
        instructions = [
            {"address": address + i, "mnemonic": mnemonic, "operands": [], "length": 1}
            for i, mnemonic in enumerate(["mov", "add", "xor", "cmp", "ret"])
        ]
        result.append({
            "address": address,
            "name": "root" if index == 0 else f"sub_{address:x}",
            "size": 5,
            "instructions": instructions,
            "cfg_hash": "single-block",
            "call_graph_hash": "chain",
            "callers": [base + (index - 1) * 32] if index else [],
            "callees": [base + (index + 1) * 32] if index + 1 < count else [],
            "basic_blocks": [{
                "address": address,
                "instructions": instructions,
                "mnemonic_hash": "identical",
                "instruction_count": 5,
            }],
        })
    return json.dumps(result).encode()


count = int(sys.argv[2]) if len(sys.argv) > 2 else 1000
a, b = payload(0x1000, count), payload(0x100000, count)
times = []
mapping_digest = None
for _ in range(3):
    start = time.perf_counter()
    pointer = library.rust_diff_diff_json(a, b)
    if not pointer:
        raise RuntimeError("engine returned null")
    try:
        result = json.loads(ctypes.string_at(pointer))
    finally:
        library.rust_diff_free_string(pointer)
    assert len(result["matched_functions"]) == count
    assert all(
        match["function_a"]["address"] - 0x1000
        == match["function_b"]["address"] - 0x100000
        for match in result["matched_functions"]
    )
    mapping = sorted(
        (match["function_a"]["address"], match["function_b"]["address"])
        for match in result["matched_functions"]
    )
    digest = hashlib.sha256(json.dumps(mapping).encode()).hexdigest()
    if mapping_digest is not None:
        assert digest == mapping_digest
    mapping_digest = digest
    times.append(time.perf_counter() - start)
print(json.dumps({
    "engine": sys.argv[1],
    "case": "duplicate bodies with one named call-graph anchor",
    "function_count": count,
    "mapping_sha256": mapping_digest,
    "seconds": times,
    "median_seconds": statistics.median(times),
}, indent=2))
