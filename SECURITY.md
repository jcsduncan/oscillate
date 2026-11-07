# Security policy

Oscillate is pre-release software. Only the current `main` branch receives
fixes.

## Reporting a vulnerability

Report vulnerabilities privately through GitHub's
[security advisories](https://docs.github.com/en/code-security/security-advisories/guidance-on-reporting-and-writing-information-about-vulnerabilities/privately-reporting-a-security-vulnerability)
("Report a vulnerability" on the repository's Security tab). Do not open a
public issue. Include the affected commit, a description of the problem, and
a minimal reproducing input if you have one.

You should receive an acknowledgement within a week. Please allow time for a
fix before disclosing the problem publicly.

## What counts as untrusted input

The application treats these as untrusted and is expected to reject or
contain malformed data without memory-safety errors, writes outside the
project directory or loss of saved work:

- **Project files:** the manifest, generation and pointer files, and managed
  assets in a project directory.
- **Media:** imported WAV, AIFF, FLAC and MP3 files, Standard MIDI Files,
  impulse responses, and installed sample libraries.
- **Plugin data:** VST3 bundles found during scanning and saved plugin state.

VST3 plugins run inside the application process once loaded. A malicious or
faulty plugin can do anything the user can; scanning in a helper process
limits crashes during discovery but is not a sandbox. Reports about a
plugin's own behaviour belong with its vendor.
