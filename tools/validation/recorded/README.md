# Optional recorded-file transport validation

This standalone CMake harness calls the actual application parser/replay/detector
sources. Supply the unchanged `ADC_DATA.bin` from the repository's recorded-data
release. Confirm its published SHA-256 first. The file is not included here.

```sh
cmake -S tools/validation/recorded -B build-recorded -DCMAKE_PREFIX_PATH=/path/to/Qt6
cmake --build build-recorded -j 4
./build-recorded/sample_qa /path/to/ADC_DATA.bin
./build-recorded/sample_unverified_qa /path/to/ADC_DATA.bin
```

The first test accelerates transport using an artificial 1 MHz setting. The second
uses arbitrary rate/gain/TDM bookkeeping settings. Neither infers true acquisition
parameters. See [the checked results](../../../docs/validation/recorded_sample_qa.md).
