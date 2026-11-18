# Fuzzing abt_core (libFuzzer + AddressSanitizer)

Targets: `fuzz_probe` (abt_probe_file + EPSI / PSRES-JPEG / base64 preview decoders),
`fuzz_psd` (PSD/PSB probe, composite, layer composite, pixbuf->BGRA), `fuzz_qoi` (QOI decode + round trip).
Inputs over 4 MB are skipped.

## Build
    build.cmd fuzz            (configures build\fuzz from the "fuzz" preset: ABT_FUZZ=ON builds abt_core + fuzzers only)

## Seeds (kept out of git, under build\fuzz\seeds)
    sh tests/fuzz/make_seeds.sh     (needs tests/corpus/generated; files > 300 KB are truncated to 256 KB)

## Run (ASan DLL must be on PATH)
    set PATH=%ProgramFiles%\Microsoft Visual Studio\18\Community\VC\Tools\MSVC\<ver>\bin\Hostx64\x64;%PATH%
    mkdir build\fuzz\corp\psd build\fuzz\art
    build\fuzz\bin\fuzz_psd.exe -max_total_time=240 -max_len=2000000 -timeout=10 -rss_limit_mb=2048 ^
        -artifact_prefix=build\fuzz\art\psd_ build\fuzz\corp\psd build\fuzz\seeds\psd

Reproduce a crash: `fuzz_psd.exe build\fuzz\art\psd_crash-...`. A >10 s input is reported as a timeout (hang).
