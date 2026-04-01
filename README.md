# SDE C++ Reproduction (AAAI 2012)

This project reproduces the core algorithmic pipeline from:

- Roni Stern, Meir Kalech, Alexander Feldman, Gregory Provan
- "Exploring the Duality in Conflict-Directed Model-Based Diagnosis"
- AAAI 2012

## Implemented Method

- Primal-Dual Diagnostic Search (PDDS)
- Switching Diagnostic Engine (SDE): alternating conflict search and diagnosis search
- SAT-based consistency checking with MiniSat
- Weak-fault model encoding: healthy gate implies nominal logic, faulty gate unconstrained

## Build

From this directory:

```bash
make
```

If `minisat` is missing:

```bash
sudo apt-get install -y minisat
```

## Run

```bash
./sde_solver <bench_file> <observation_file> [--first] [--timeout <sec>]
```

- `--first`: stop after first found diagnosis (anytime mode)
- `--timeout <sec>`: hard timeout in seconds (default: 30)

Example:

```bash
./sde_solver ../iscas85/bench/c1908.bench generated_obs/c1908/c1908_obs_0.txt --first --timeout 30
```

## Generate Observation Sets (Single/Double Fault)

Build:

```bash
make obs_generator
```

Generate 20 cases for c17 with exactly 2 faults per case:

```bash
./obs_generator ../iscas85/bench/c17.bench generated_obs/c17 20 2 123
```

Parameters:

- `bench_file`: ISCAS85 `.bench` circuit
- `out_dir`: output folder for observation files
- `num_cases`: number of observation files
- `fixed_faults`: fixed injected fault count per case
- `seed` (optional): random seed for reproducibility

Generated observations are filtered so that at least one primary output differs from the all-healthy simulation for the same primary inputs. This avoids trivial empty-diagnosis cases caused by fault masking.

## Observation File Format

One Boolean assignment per line:

```txt
# example
N1=0
N2=1
N22=1
N23=0
```

You can constrain primary inputs, primary outputs, and/or internal signals.

## Notes

- The benchmark folder provides circuit structure only (`.bench`).
- To reproduce paper-style diagnosis experiments exactly, you also need an observation set generator or the original observation corpus.
- This implementation is complete for the algorithmic core and SAT-based inference path in C++.
