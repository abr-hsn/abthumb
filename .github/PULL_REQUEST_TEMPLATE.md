## Summary
<!-- Describe what this PR changes and why. Link any related issue (e.g. Closes #123). -->

## Verification
- [ ] `build.cmd release` builds cleanly with `/W4 /WX`
- [ ] `abt_tests.exe` passes with 0 failures
- [ ] `powershell -File scripts\ci.ps1 -Quick` passes (`cppcheck`, formatting check)
- [ ] For parser changes in `src/core`: added unit test(s) and ran libFuzzer + ASan
