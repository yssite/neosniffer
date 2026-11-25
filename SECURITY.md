# Security Policy

## Supported Versions

| Version | Supported |
|---|---|
| 1.1.x | Yes |

## Running as Administrator

NeoSniffer does **not** require administrator privileges for normal use. Elevation is strictly optional and is only used to open raw NTFS volume handles (`\\.\X:`) for fast sequential `$MFT` scanning (`FSCTL_GET_NTFS_VOLUME_DATA`).

When running NeoSniffer elevated (via **Restart as Admin** or **Always run as administrator**), please keep the following security considerations in mind:

1. **Least privilege by default:** Standard user mode uses parallel `FindFirstFileExW` directory enumeration and never opens raw disk devices. Prefer non-elevated mode unless you specifically need `$MFT` speed or access to protected system folders.
2. **Untrusted files:** Avoid opening untrusted snapshot files (`.ssx`, `.sns`, `.du`) or untrusted XML configuration files while running as Administrator.
3. **Shell context menu:** Invoking the system Explorer context menu from inside an elevated NeoSniffer process may launch third-party shell extensions or target files with elevated privileges.
4. **Automatic fallback:** If UAC is declined or raw volume access fails, NeoSniffer falls back to standard directory enumeration without retrying elevation.

## MCP Server (`NeoSniffer.Mcp.exe`)

- All analysis tools are **read-only** and never delete, rename, or modify scanned files.
- `export_report` returns report text through stdio. Upgrade clients using 1.0.x, whose report tool accepted a caller-supplied filesystem destination.
- `NeoSniffer.Mcp.exe` communicates strictly over local `stdio` and does not open listening network sockets.

## Reporting a Vulnerability

Please **do not** report security vulnerabilities through public GitHub issues.

Instead, report them privately via GitHub Security Advisories:

- **GitHub Advisory:** https://github.com/yssite/neosniffer/security/advisories/new

Please include:
- A clear description of the vulnerability and potential impact
- Steps to reproduce or a minimal proof-of-concept (e.g., malformed snapshot, MFT record, or MCP JSON-RPC input)
- Affected version and Windows build

We aim to acknowledge reports within **5 business days** and provide a remediation plan or patch timeline once verified.
