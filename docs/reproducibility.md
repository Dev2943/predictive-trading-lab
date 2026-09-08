# Reproducibility

## The fingerprints

```bash
./apps/ptl_version -c config/base.toml
```

```
config hash            30b44e5972450aad
rng[0..2] from seed    d05ef55272cdfb14 2e2f422341add64e 1c120f3d1ce63170
```

These have been stable across every phase of this project, on GCC 13 and
Clang 18, in Debug, Release, sanitized, C++20 and C++23 builds. A change in
either value means something in the toolchain or the code is not reproducing the
reference build.

## What guarantees them

| Mechanism | What it prevents |
|---|---|
| `DeterministicRng` | Standard library `<random>` distributions differ between implementations for the same seed |
| UTC nanoseconds only | Timezone database version changes shifting timestamps |
| No wall clock on the trading path | Runs that differ because they happened at different times |
| Ordered containers (`std::map`) in output | Serialization order varying with hash seeds |
| Fixed serialization precision | Locale- or library-dependent float formatting |
| Single-threaded | Interleaving that varies run to run |
| `PTL_NATIVE_ARCH=OFF` by default | `-march=native` changing floating-point results per machine |

## What breaks them

- **`-march=native` or `-ffast-math`.** Different instruction selection gives
  different rounding. The option exists and defaults off for this reason.
- **Changing iteration order** anywhere output is accumulated. Float summation
  is not associative.
- **Reading the wall clock** in a component on the trading path.
- **A different Eigen version** may change the last bits of a decomposition. The
  model layer is the least portable part of the system.

## Why exact comparisons

Several tests use `==` on doubles rather than a tolerance. An approximate
comparison would pass through exactly the reordering these tests exist to catch:
if summation order changes, the last bit changes, and `Approx` would call that
equal. The exact comparison is the test.

## Reporting a mismatch

Include your compiler and version, OS and architecture, CMake preset, Eigen
version, and both fingerprint lines. The first three explain most of them.
