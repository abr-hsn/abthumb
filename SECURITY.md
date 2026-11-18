# Security policy

abthumb's shell extension is loaded into Windows Explorer and the preview host, and its render
workers parse untrusted Illustrator, EPS, Photoshop and PDF files. Security reports are very
welcome.

## Reporting a vulnerability
Please use GitHub's private vulnerability reporting ("Report a vulnerability" on the Security tab
of this repository) instead of a public issue. Include the abthumb version, the Windows build, and,
if possible, a minimal file that triggers the problem. You should get an answer within a week.

## Supported versions
Only the latest release gets security fixes.

## Design summary
* Explorer side: `abthumb.dll` never parses file content. It sends the path over a per-user named
  pipe and copies the returned pixels into a bitmap, after validating every size field.
* `abthumb-agent.exe` (per user) owns the cache and scheduling and never parses file content.
  Callers below medium integrity can only request images.
* `abthumb-render.exe` workers do all parsing and rendering inside a job object (memory cap,
  kill on close, per-request deadline). Their replies are validated like any untrusted input.
* Parsers are fuzzed with libFuzzer and AddressSanitizer (`tests/fuzz`).

Details and known limitations: [DECISIONS.md](DECISIONS.md), entries D-005 and D-028.
