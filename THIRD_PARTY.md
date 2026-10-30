# Third-party dependencies and licensing

RyzeRTC is an original RyzeCompany PHP extension; it relies on a pre-existing low-level WebRTC library rather than implementing the entire WebRTC stack from scratch.

| Dependency | Pinned version | License |
| --- | --- | --- |
| libdatachannel | v0.24.6, commit `6b1e2e620f1e37f0eafeee702eaea0043cb305fd` | MPL-2.0 |
| libjuice | libdatachannel's pinned submodule | MPL-2.0 |
| usrsctp | libdatachannel's pinned submodule | BSD-3-Clause |
| plog | libdatachannel's pinned submodule | MIT |
| nlohmann/json | libdatachannel's pinned submodule | MIT |
| OpenSSL | system/OS runtime dependency | Apache-2.0 |

Original RyzeRTC code is MIT licensed, Copyright (c) 2026 RyzeCompany. A library linked into RyzeRTC is **not** re-licensed as RyzeRTC or RyzeCompany software. Before a stable release: package the full upstream license notices, maintain compliance with MPL source-availability requirements and enumerate all actual linked shared libraries. See https://github.com/paullouisageneau/libdatachannel.
