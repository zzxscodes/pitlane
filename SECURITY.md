# Security

pitlane is a same-machine helper library. It opens no exchange or broker sessions; the only socket helper is the optional pitlane-to-pitlane UDP link.

Report vulnerabilities privately to the repository owner on GitHub (Security advisory preferred). Include reproduction steps and affected version (`pitlane::kVersion`).

Do not file public issues with exploit details for privileged host helpers (`mlockall`, IRQ affinity, hugepage sysfs) until a fix is available.
