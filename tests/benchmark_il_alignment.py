"""Check large IL alignment output and cost in a fresh process per library.

Usage: python3 tests/benchmark_il_alignment.py LIBRARY [CASE]
"""

import ctypes
import hashlib
import json
import statistics
import sys
import time


def line(text):
    return {"text": text}


def request(case):
    lines = [line(str(i)) for i in range(2100)]
    if case == "equal":
        a, b = lines, lines
    elif case == "shifted":
        a, b = lines, [line("inserted")] + lines
    elif case == "unrelated-skew":
        a = [line("left")] * 512
        b = [line("right")] * 65536
    elif case == "token-shifted":
        tokens = [{"kind": "integer", "text": str(i)} for i in range(257)]
        shifted = [{"kind": "integer", "text": "inserted"}] + tokens
        a = [{"text": " ".join(t["text"] for t in tokens), "tokens": tokens}]
        b = [{"text": " ".join(t["text"] for t in shifted), "tokens": shifted}]
    else:
        raise ValueError(f"unknown case: {case}")
    return json.dumps({"a": {"lines": a}, "b": {"lines": b}}).encode()


def validate(case, result):
    ops = [line["op"] for line in result["lines"]]
    if case == "equal":
        assert result["similarity"] == 1.0 and ops == ["equal"] * 2100
    elif case == "shifted":
        assert ops == ["insert"] + ["equal"] * 2100
        assert abs(result["similarity"] - 2100 / 2101) < 1e-12
    elif case == "unrelated-skew":
        assert result["similarity"] == 0.0
        assert ops == ["replace"] * 512 + ["insert"] * (65536 - 512)
    else:
        assert ops == ["replace"]
        assert all(not span["changed"] for span in result["lines"][0]["a_spans"])
        assert sum(span["changed"] for span in result["lines"][0]["b_spans"]) == 1


def main():
    library = ctypes.CDLL(sys.argv[1])
    library.rust_diff_il_diff_json.argtypes = [ctypes.c_char_p]
    library.rust_diff_il_diff_json.restype = ctypes.c_void_p
    library.rust_diff_free_string.argtypes = [ctypes.c_void_p]
    library.rust_diff_free_string.restype = None
    cases = sys.argv[2:] or ["equal", "shifted", "unrelated-skew", "token-shifted"]
    for case in cases:
        payload = request(case)
        times = []
        digest = None
        for _ in range(3):
            start = time.perf_counter()
            pointer = library.rust_diff_il_diff_json(payload)
            if not pointer:
                raise RuntimeError("engine returned null")
            try:
                result = json.loads(ctypes.string_at(pointer))
            finally:
                library.rust_diff_free_string(pointer)
            times.append(time.perf_counter() - start)
            validate(case, result)
            result_digest = hashlib.sha256(json.dumps(result, sort_keys=True).encode()).hexdigest()
            if digest is not None:
                assert digest == result_digest
            digest = result_digest
        print(json.dumps({
            "case": case,
            "result_sha256": digest,
            "seconds": times,
            "median_seconds": statistics.median(times),
        }))


if __name__ == "__main__":
    main()
