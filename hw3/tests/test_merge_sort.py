#!/usr/bin/env python3
"""
Black-box test bench for the parallel merge sort (HW3).

The program is treated as a black box. This harness only relies on its I/O:

  - reads "input.txt" from its working directory: N, then N integers
  - writes "output_1.txt" ... "output_8.txt" (one per worker-thread count),
    the sorted integers separated by whitespace
  - prints "worker thread #k, elapsed X ms" once per worker-thread count

Every run happens in a fresh temp directory, so your own input.txt / output
files are never touched.

Three groups of tests:

  CORRECTNESS  edge cases (N not divisible by 8, N < 8, duplicates, ...)
  STRESS       the same small inputs many times, to catch races and hangs
               that only show up occasionally
  BENCHMARK    one large random input, checked for correctness and timed

Usage:
    python3 tests/test_merge_sort.py                   # everything
    python3 tests/test_merge_sort.py --large 200000    # bigger benchmark
    python3 tests/test_merge_sort.py --skip-bench      # correctness + stress
    python3 tests/test_merge_sort.py --gen 100000 > input.txt
"""

import argparse
import os
import random
import re
import shutil
import subprocess
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
PROJECT_DIR = os.path.dirname(HERE)

WORKERS = range(1, 9)
TIMING_RE = re.compile(r"worker thread #(\d+), elapsed ([\d.]+) ms")

INT_MIN, INT_MAX = -(2**31), 2**31 - 1


# --------------------------------------------------------------------------
# Input generation
# --------------------------------------------------------------------------

def random_values(rng, n, lo=0, hi=1_000_000):
    return [rng.randint(lo, hi) for _ in range(n)]


def format_input(values):
    return f"{len(values)}\n" + " ".join(map(str, values)) + "\n"


# --------------------------------------------------------------------------
# Running the program and checking outputs
# --------------------------------------------------------------------------

class RunFailure(Exception):
    pass


def run_program(binary, values, timeout):
    """Runs the program on `values` in a fresh temp dir.
    Returns (outputs, timings, wall_seconds) where outputs maps k -> list of
    ints (or None if output_k.txt is missing) and timings maps k -> ms."""
    workdir = tempfile.mkdtemp(prefix="merge_sort_test_")
    try:
        with open(os.path.join(workdir, "input.txt"), "w") as f:
            f.write(format_input(values))

        start = time.monotonic()
        try:
            r = subprocess.run([binary], cwd=workdir, capture_output=True,
                               text=True, timeout=timeout)
        except subprocess.TimeoutExpired:
            raise RunFailure(f"timed out after {timeout:.0f}s (hang / deadlock?)")
        wall = time.monotonic() - start

        if r.returncode < 0:
            raise RunFailure(f"killed by signal {-r.returncode} "
                             f"(e.g. 11 = segfault, 6 = abort)")
        if r.returncode != 0:
            raise RunFailure(f"exit code {r.returncode}\nstderr: {r.stderr.strip()}")

        timings = {int(k): float(ms) for k, ms in TIMING_RE.findall(r.stdout)}

        outputs = {}
        for k in WORKERS:
            path = os.path.join(workdir, f"output_{k}.txt")
            if not os.path.exists(path):
                outputs[k] = None
                continue
            with open(path) as f:
                tokens = f.read().split()
            try:
                outputs[k] = [int(t) for t in tokens]
            except ValueError:
                raise RunFailure(f"output_{k}.txt contains a non-integer token")
        return outputs, timings, wall
    finally:
        shutil.rmtree(workdir, ignore_errors=True)


def describe_mismatch(got, expected):
    if len(got) != len(expected):
        return f"expected {len(expected)} numbers, got {len(got)}"
    if sorted(got) != expected:
        return "numbers differ from the input (some lost, duplicated or corrupted)"
    i = next(i for i in range(len(got)) if got[i] != expected[i])
    return (f"same numbers but not sorted: first wrong position is index {i} "
            f"(got {got[i]}, expected {expected[i]})")


def check_outputs(outputs, values):
    """Returns a list of problems, one per bad output file."""
    expected = sorted(values)
    problems = []
    for k in WORKERS:
        got = outputs[k]
        if got is None:
            problems.append(f"output_{k}.txt missing")
        elif got != expected:
            problems.append(f"output_{k}.txt ({k} workers): "
                            + describe_mismatch(got, expected))
    return problems


def check_timings(timings):
    missing = [k for k in WORKERS if k not in timings]
    if missing:
        return [f"no 'worker thread #k, elapsed X ms' line for k = {missing}"]
    return []


# --------------------------------------------------------------------------
# Test cases
# --------------------------------------------------------------------------

class TestResult:
    def __init__(self, name, category, passed, message=""):
        self.name = name
        self.category = category
        self.passed = passed
        self.message = message


RESULTS = []


def correctness_cases(rng):
    """(name, values) pairs. Sizes cover N % 8 == 0, N % 8 != 0 and N < 8,
    since the array is split into 8 chunks."""
    return [
        ("N = 16, reverse sorted (N % 8 == 0)", list(range(16, 0, -1))),
        ("N = 96, random (N % 8 == 0)", random_values(rng, 96)),
        ("N = 10, random (N % 8 == 2)", random_values(rng, 10)),
        ("N = 15, random (N % 8 == 7)", random_values(rng, 15)),
        ("N = 1003, random (N % 8 == 3)", random_values(rng, 1003)),
        ("N = 8, random (one number per chunk)", random_values(rng, 8)),
        ("N = 5, random (N < 8, some chunks empty)", random_values(rng, 5)),
        ("N = 1", [42]),
        ("already sorted (N = 1000)", sorted(random_values(rng, 1000))),
        ("reverse sorted (N = 1000)", sorted(random_values(rng, 1000), reverse=True)),
        ("all equal (N = 1000)", [7] * 1000),
        ("many duplicates (N = 1000, values 0..9)", random_values(rng, 1000, 0, 9)),
        ("negative numbers (N = 1000)", random_values(rng, 1000, -1_000_000, 1_000_000)),
        ("INT_MIN / INT_MAX (N = 100)",
         random_values(rng, 98, -1000, 1000) + [INT_MIN, INT_MAX]),
        ("medium random (N = 20000)", random_values(rng, 20000)),
    ]


def run_case(binary, name, category, values, timeout):
    try:
        outputs, timings, _ = run_program(binary, values, timeout)
    except RunFailure as e:
        RESULTS.append(TestResult(name, category, False, str(e)))
        return
    problems = check_outputs(outputs, values) + check_timings(timings)
    RESULTS.append(TestResult(name, category, not problems, "\n".join(problems)))


def run_stress(binary, rng, repeat, timeout):
    """Each run of the program already exercises 1..8 workers. Repeating it
    makes rare interleavings (lost jobs, stale jobs, missed wake-ups) likely
    to show up at least once."""
    for n in (16, 96, 1003):
        name = f"N = {n}, {repeat} runs with fresh random input"
        failures = []
        for i in range(repeat):
            values = random_values(rng, n)
            try:
                outputs, timings, _ = run_program(binary, values, timeout)
            except RunFailure as e:
                failures.append(f"run {i + 1}: {e}")
                continue
            problems = check_outputs(outputs, values) + check_timings(timings)
            if problems:
                failures.append(f"run {i + 1}: " + "; ".join(problems))
        message = ""
        if failures:
            shown = failures[:3]
            more = len(failures) - len(shown)
            message = (f"{len(failures)}/{repeat} runs failed\n"
                       + "\n".join(shown)
                       + (f"\n... and {more} more" if more else ""))
        RESULTS.append(TestResult(name, "STRESS", not failures, message))


def run_benchmark(binary, rng, n, timeout):
    name = f"large random input (N = {n})"
    print(f"Running benchmark with N = {n} (this can take a while)...")
    values = random_values(rng, n)
    try:
        outputs, timings, wall = run_program(binary, values, timeout)
    except RunFailure as e:
        RESULTS.append(TestResult(name, "BENCHMARK", False, str(e)))
        return None
    problems = check_outputs(outputs, values) + check_timings(timings)
    RESULTS.append(TestResult(name, "BENCHMARK", not problems, "\n".join(problems)))
    return timings, wall


def print_timing_table(n, timings, wall):
    print(f"== TIMING (N = {n}) " + "=" * (60 - len(str(n)) - 13))
    if 1 not in timings:
        print("no timing lines found in the program's stdout\n")
        return
    base = timings[1]
    print(f"{'workers':>8}  {'elapsed (ms)':>14}  {'speedup vs 1':>12}")
    for k in WORKERS:
        if k in timings:
            speedup = base / timings[k] if timings[k] > 0 else float("inf")
            print(f"{k:>8}  {timings[k]:>14.3f}  {speedup:>11.2f}x")
    print(f"(whole program wall time: {wall:.2f}s)")
    print("With 8 equal sort jobs, expect the time to track the number of rounds\n"
          "needed: 1->8, 2->4, 3->3, 4..7->2, 8->1 (roughly flat from 4 to 7).\n")


# --------------------------------------------------------------------------
# Runner
# --------------------------------------------------------------------------

def resolve_binary(args):
    if args.binary:
        if not os.path.exists(args.binary):
            print(f"error: binary not found: {args.binary}")
            sys.exit(1)
        return os.path.abspath(args.binary)

    src = args.source or os.path.join(PROJECT_DIR, "merge_sort_parallel.c")
    if not os.path.exists(src):
        print(f"error: source not found: {src}")
        sys.exit(1)

    build_dir = tempfile.mkdtemp(prefix="merge_sort_build_")
    tmp_bin = os.path.join(build_dir, "merge_sort_under_test")
    cmd = ["gcc", *args.cflags.split(), "-pthread", "-o", tmp_bin, src]
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode != 0:
        print("Compilation failed:\n" + r.stderr)
        sys.exit(1)
    print(f"[built with: {' '.join(cmd)}]")
    if r.stderr.strip():
        print("[compiler warnings]\n" + r.stderr.rstrip())
    return tmp_bin


def main():
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--binary", help="path to an already-built executable")
    parser.add_argument("--source", help="path to the .c file to compile "
                        "(default: ../merge_sort_parallel.c)")
    parser.add_argument("--cflags", default="-O2 -Wall",
                        help="compiler flags (default: '-O2 -Wall')")
    parser.add_argument("--seed", type=int, help="random seed (default: random, printed)")
    parser.add_argument("--repeat", type=int, default=30,
                        help="runs per stress test (default: 30)")
    parser.add_argument("--large", type=int, default=100_000,
                        help="N for the benchmark (default: 100000)")
    parser.add_argument("--timeout", type=float, default=10,
                        help="seconds per run before calling it a hang (default: 10)")
    parser.add_argument("--bench-timeout", type=float, default=600,
                        help="seconds for the benchmark run (default: 600)")
    parser.add_argument("--skip-bench", action="store_true", help="skip the benchmark")
    parser.add_argument("--gen", type=int, metavar="N",
                        help="only print a random input of size N to stdout, then exit")
    args = parser.parse_args()

    seed = args.seed if args.seed is not None else random.randrange(2**32)
    rng = random.Random(seed)

    if args.gen is not None:
        sys.stdout.write(format_input(random_values(rng, args.gen)))
        return

    binary = resolve_binary(args)
    print(f"Running tests against: {binary}")
    print(f"Seed: {seed}  (rerun with --seed {seed} to get the same inputs)\n")

    for name, values in correctness_cases(rng):
        run_case(binary, name, "CORRECTNESS", values, args.timeout)
    run_stress(binary, rng, args.repeat, args.timeout)

    bench = None
    if not args.skip_bench:
        bench = run_benchmark(binary, rng, args.large, args.bench_timeout)
    print()

    for cat in ("CORRECTNESS", "STRESS", "BENCHMARK"):
        cat_results = [r for r in RESULTS if r.category == cat]
        if not cat_results:
            continue
        print(f"== {cat} " + "=" * (60 - len(cat)))
        for r in cat_results:
            status = "PASS" if r.passed else "FAIL"
            print(f"[{status}] {r.name}")
            if not r.passed:
                for line in r.message.splitlines():
                    print(f"       -> {line}")
        print()

    if bench:
        print_timing_table(args.large, *bench)

    total = len(RESULTS)
    passed = sum(1 for r in RESULTS if r.passed)
    print("=" * 64)
    print(f"TOTAL: {passed}/{total} passed")

    sys.exit(0 if passed == total else 1)


if __name__ == "__main__":
    main()
