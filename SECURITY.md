# Security Policy

## Supported Versions

| Version | Supported          |
| :------ | :----------------- |
| 0.1.x   | :white_check_mark: |

## Reporting Vulnerabilities

Report security vulnerabilities privately through the repository's
[GitHub Security Advisories form](https://github.com/nemarpuc/libmimhid/security/advisories/new).
Do not include serial numbers, hardware MAC addresses, or raw USB packets containing
real user input in public issues.

## Sensitive Input Protection

Input reports can contain sensitive information, including keystrokes, credentials,
touch coordinates, and accessibility actions. `libmimhid` does not print input
report payloads to its standard log outputs. Applications and tests using `libmimhid`
should never write raw report buffers into persistent application logs, crash reports,
or public bug trackers; reproduce bugs with synthetic, non-sensitive input.

## System Device Permissions

Serial device nodes (`/dev/ttyUSB*`, `/dev/ttyACM*` on Linux, `/dev/cu.*` on macOS) and COM ports
on Windows are privileged system resources. Any process that can open the port can inject keystrokes
and touch input into the target, so restrict access to trusted users. On Linux, the rules provided in `udev/99-mimhid.rules` configure
desktop seat permissions (`TAG+="uaccess"`) and group access (mode `0660`) for specific USB-UART bridge IDs only. Audit and review
these rules against your target environment's access control policies before deployment.
