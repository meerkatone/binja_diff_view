"""Measure labelled function matching on a saved real Binary Ninja extraction.

Usage: python3 tests/accuracy_memcpy_fixture.py /path/to/librust_diff.dylib
The two JSON inputs and the independent address labels are committed fixtures.
"""

import ctypes
import json
from pathlib import Path
import sys


def diff(library, left, right):
    pointer = library.rust_diff_diff_json(left, right)
    if not pointer:
        raise RuntimeError("Rust engine returned a null result")
    try:
        return json.loads(ctypes.string_at(pointer))
    finally:
        library.rust_diff_free_string(pointer)


def main(path):
    root = Path(__file__).parent / "fixtures"
    manifest = json.loads((root / "memcpy_accuracy_manifest.json").read_text())
    left = (root / manifest["reference"]).read_bytes()
    right = (root / manifest["target"]).read_bytes()
    expected = {(int(pair["a"], 16), int(pair["b"], 16)) for pair in manifest["expected_pairs"]}
    left_addresses = {function["address"] for function in json.loads(left)}
    right_addresses = {function["address"] for function in json.loads(right)}
    assert all(a in left_addresses and b in right_addresses for a, b in expected)

    library = ctypes.CDLL(str(Path(path).resolve()))
    library.rust_diff_diff_json.argtypes = [ctypes.c_char_p, ctypes.c_char_p]
    library.rust_diff_diff_json.restype = ctypes.c_void_p
    library.rust_diff_free_string.argtypes = [ctypes.c_void_p]
    library.rust_diff_free_string.restype = None

    forward = diff(library, left, right)
    reverse = diff(library, right, left)
    actual = {(m["function_a"]["address"], m["function_b"]["address"])
              for m in forward["matched_functions"]}
    reversed_actual = {(m["function_b"]["address"], m["function_a"]["address"])
                       for m in reverse["matched_functions"]}
    correct = actual & expected
    wrong = actual - expected
    missed = expected - actual
    hints = {(hint["a_address"], hint["b_address"])
             for hint in forward.get("review_suggestions", [])}
    reverse_hints = {(hint["b_address"], hint["a_address"])
                     for hint in reverse.get("review_suggestions", [])}
    result = {
        "correct": len(correct),
        "wrong": len(wrong),
        "missed": len(missed),
        "precision": len(correct) / len(actual) if actual else 0.0,
        "recall": len(correct) / len(expected),
        "direction_symmetric": actual == reversed_actual,
        "review_hints_for_missed": len(missed & hints),
        "hint_direction_symmetric": hints == reverse_hints,
        "missed_identities": [pair["identity"] for pair in manifest["expected_pairs"]
                              if (int(pair["a"], 16), int(pair["b"], 16)) in missed],
    }
    print(json.dumps(result, indent=2))
    return int(bool(wrong) or len(correct) < manifest["minimum_correct_pairs"]
               or actual != reversed_actual or hints != reverse_hints)


if __name__ == "__main__":
    if len(sys.argv) != 2:
        raise SystemExit(__doc__)
    raise SystemExit(main(sys.argv[1]))
