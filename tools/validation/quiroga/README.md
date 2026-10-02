# Optional published-data detector check

This independent harness calls the application's actual `SpikeProcessor` and does
not alter its thresholds. Dependencies: Linux GCC, Qt6 Core headers/libraries,
Python with NumPy and SciPy, and curl. It downloads the 258 MB author-published
Quiroga archive, verifies its official checksum, and credits the CC BY 4.0 source.
It does not run as part of the short CTest suite.

```sh
export NL_DEPS_PREFIX=/path/to/qt6/usr
export LD_LIBRARY_PATH="$NL_DEPS_PREFIX/lib/x86_64-linux-gnu:${LD_LIBRARY_PATH:-}"
bash tools/validation/quiroga/reproduce.sh
```

The checked run's complete method and results are in
[docs/validation/quiroga](../../../docs/validation/quiroga/README.md).
No raw benchmark traces are committed. This checks single-channel threshold
crossings, not biological unit isolation or hardware readiness.
