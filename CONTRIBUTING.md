# Contributing to abthumb

Thanks for helping. abthumb runs inside Windows Explorer and parses untrusted files, so the bar
for safety is high and the code style is deliberately plain.

## Before you start
* Read [DECISIONS.md](DECISIONS.md) (why things are the way they are), [docs/SPEC.md](docs/SPEC.md)
  and [docs/PLAN.md](docs/PLAN.md). Please open an issue before large changes, especially to
  anything listed in DECISIONS.md.
* Security problems: please do not open a public issue, see [SECURITY.md](SECURITY.md).

## Build and test
Requirements: Windows 10/11 x64, Visual Studio 2022 or newer with the C++ workload (CMake and Ninja
come with it).

```
build.cmd release                      :: build\release\bin
cd %TEMP% && <repo>\build\release\bin\abt_tests.exe
build.cmd fuzz                         :: libFuzzer + ASan targets, see tests/fuzz/README.md
```

Unit tests that need the generated corpus skip themselves when it is missing. The corpus is
generated from Illustrator and Photoshop (see `tests/corpus/scripts/README.md`); please never commit
real design files or their names.

To try a build in Explorer, register it per user with `regsvr32 build\release\bin\abthumb.dll`
(developer mode; Explorer itself needs the machine-wide install, see D-023), or build the installer
with `powershell -File scripts\package.ps1`.

## Code style
* Format parsers in `src/core` are portable C11 with no Windows headers; everything else is C++20
  written in a C-like style: no exceptions, no RTTI, no STL containers in hot paths, hand-rolled
  COM, static CRT (D-001).
* 4-space indent, 120 columns, `snake_case` functions, comments only where the reason is not
  obvious.
* Every length, offset and count read from a file, a pipe or the cache is untrusted: check it
  before use, do size math in 64 bits, and cap allocations.
* Builds must stay warning-free at `/W4`.

## Pull requests
* One topic per PR, with tests for parser changes (a small synthetic buffer in `tests/unit` is
  ideal) and a short fuzz run for anything in `src/core`.
* CI must be green (build, unit tests, fuzz smoke run).
* By contributing you agree that your contribution is licensed under the MIT licence of this
  project.
