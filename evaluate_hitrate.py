#!/usr/bin/env python3
"""
Evaluate fault hit rate for SDE or CDA solver.

Hit rate = |diagnosis ∩ true_faults| / |diagnosis|

Usage:
  python3 evaluate_hitrate.py <bench> <obs_file> <solver_exe> [--timeout N]
  python3 evaluate_hitrate.py <bench> <obs_dir> <solver_exe> [--timeout N] [--batch]
"""

import sys
import os
import re
import time
import subprocess
import argparse
import json


def parse_true_faults(obs_path: str) -> set:
    """Read the # faults= line from an observation file."""
    with open(obs_path) as f:
        for line in f:
            if line.startswith("# faults="):
                parts = line.strip().split("=", 1)
                if len(parts) == 2 and parts[1].strip():
                    return set(g.strip() for g in parts[1].split(","))
    return set()


def parse_diagnosis_output(output: str):
    """
    Parse solver output to extract diagnosis component sets.
    Returns list of sets, e.g. [{'381'}, {'213', '333'}]

    Supports formats:
      {381}
      {213, 333}
      1 diagnoses: {381}
      Diagnosis: {381}
    """
    diagnoses = []
    for line in output.splitlines():
        line = line.strip()
        # Try to find {...} anywhere on the line
        m = re.findall(r'\{([^}]*)\}', line)
        for content in m:
            content = content.strip()
            if content:
                components = set(g.strip() for g in content.split(",") if g.strip())
                if components:
                    diagnoses.append(components)
    return diagnoses


def run_solver(bench: str, obs: str, solver_exe: str, timeout: int = 30) -> tuple:
    """Run solver and return (stdout+stderr, elapsed_seconds)."""
    t0 = time.perf_counter()
    result = subprocess.run(
        [solver_exe, bench, obs, "--first", "--timeout", str(timeout)],
        capture_output=True, text=True, timeout=timeout + 10,
    )
    elapsed = time.perf_counter() - t0
    return result.stdout + result.stderr, elapsed


def compute_hit_rate(diagnosis: set, true_faults: set) -> tuple:
    """Return (hit_rate, hit_count, diag_size).
    If diagnosis is empty (cardinality 0 / all-healthy), hit_rate is None
    (fault not observable).
    """
    if not diagnosis:
        return None, 0, 0
    intersection = diagnosis & true_faults
    return len(intersection) / len(diagnosis), len(intersection), len(diagnosis)


def evaluate_single(bench: str, obs: str, solver_exe: str, timeout: int = 30):
    """Evaluate a single observation file."""
    true_faults = parse_true_faults(obs)
    output, elapsed = run_solver(bench, obs, solver_exe, timeout)
    diagnoses = parse_diagnosis_output(output)

    # Extract metadata from output
    status = "ok"
    minimal_card = None

    # Detect errors
    if re.search(r'Error:\s*Timeout', output):
        status = "timeout"
    elif re.search(r'^Error:', output, re.MULTILINE):
        status = "error"

    for line in output.splitlines():
        m = re.search(r"Minimal cardinality\s*=\s*(\d+)", line)
        if m:
            minimal_card = int(m.group(1))

    # Use the first diagnosis (smallest cardinality) for hit rate
    best_diag = diagnoses[0] if diagnoses else set()
    hit_rate, hit_count, diag_size = compute_hit_rate(best_diag, true_faults)

    return {
        "obs": os.path.basename(obs),
        "method": os.path.basename(solver_exe),
        "status": status,
        "true_faults": sorted(true_faults),
        "diagnosis": sorted(best_diag) if best_diag else [],
        "diagnosis_count": diag_size,
        "hit_count": hit_count,
        "hit_rate": hit_rate,  # None if empty diagnosis
        "minimal_cardinality": minimal_card,
        "time_s": elapsed,
        "raw_output": output,
    }


def main():
    parser = argparse.ArgumentParser(description="Evaluate fault hit rate")
    parser.add_argument("bench", help="Path to .bench circuit file")
    parser.add_argument("obs", help="Path to observation file or directory")
    parser.add_argument("solver", help="Path to solver executable (sde_solver or cda_solver)")
    parser.add_argument("--timeout", type=int, default=30, help="Timeout in seconds")
    parser.add_argument("--batch", action="store_true", help="If set, obs is a directory of obs files")
    parser.add_argument("--json", action="store_true", help="Output as JSON")
    args = parser.parse_args()

    solver_name = os.path.basename(args.solver)

    if args.batch:
        # Batch mode: iterate over all .txt files in directory
        obs_dir = args.obs
        if not os.path.isdir(obs_dir):
            print(f"Error: --batch mode requires a directory, got {obs_dir}", file=sys.stderr)
            sys.exit(1)
        obs_files = sorted(f for f in os.listdir(obs_dir) if f.endswith(".txt"))
        if not obs_files:
            print(f"Error: No .txt files found in {obs_dir}", file=sys.stderr)
            sys.exit(1)

        results = []
        for fname in obs_files:
            fpath = os.path.join(obs_dir, fname)
            result = evaluate_single(args.bench, fpath, args.solver, args.timeout)
            results.append(result)

        # Aggregate — three key metrics
        total = len(results)
        ok = [r for r in results if r["status"] == "ok"]
        err = [r for r in results if r["status"] in ("timeout", "error")]
        # Non-empty diagnosis: hit rate applicable
        app = [r for r in ok if r["hit_rate"] is not None]
        # Empty diagnosis (all-healthy consistent) → pseudo-normal, count as success
        masked = [r for r in ok if r["hit_rate"] is None]
        # Hit rate: only among non-empty diagnoses
        hit_rates = [r["hit_rate"] for r in app]
        avg_hit_rate = sum(hit_rates) / len(hit_rates) if hit_rates else float("nan")
        # Complete misses: non-empty diagnosis with no hits
        misses = sum(1 for r in app if r["hit_rate"] == 0.0)
        # Success: solver completed (returned any diagnosis, including empty/masked)
        # Failure: only timeout or error (solver didn't finish)
        successes = total - len(err)
        success_rate = successes / total
        # Average runtime
        all_times = [r["time_s"] for r in results]
        avg_time = sum(all_times) / total

        # Compact output
        print(f"Method: {solver_name}")
        print(f"  Avg hit rate:    {avg_hit_rate:.4f}")
        print(f"  Avg success rate:{success_rate:.4f}")
        print(f"  Avg runtime:     {avg_time:.4f} s")

        if args.json:
            # Strip raw_output to keep JSON compact
            stripped = [{k: v for k, v in r.items() if k != "raw_output"} for r in results]
            print(json.dumps(stripped, indent=2, ensure_ascii=False))
    else:
        # Single mode
        if not os.path.isfile(args.obs):
            print(f"Error: {args.obs} is not a file (use --batch for directories)", file=sys.stderr)
            sys.exit(1)
        result = evaluate_single(args.bench, args.obs, args.solver, args.timeout)

        hr_str = f"{result['hit_rate']:.4f}" if result['hit_rate'] is not None else "N/A"
        print(f"Method: {solver_name}")
        print(f"  Hit rate:        {hr_str}")
        print(f"  Runtime:         {result['time_s']:.4f} s")
        print(f"  Status:          {result['status']}")

        if args.json:
            stripped = {k: v for k, v in result.items() if k != "raw_output"}
            print(json.dumps(stripped, indent=2, ensure_ascii=False))


if __name__ == "__main__":
    main()
