# Cycle 8 Plan — Connectivity: TCP/IP (and USB)

> Vision (user): let TinyOS talk to the outside world — over **TCP/IP** and/or
> **USB**.

## Direction: TCP/IP first, USB as a later real-hardware track

- On a **VM**, the natural NIC is **paravirtual**: **virtio-net** on QEMU and
  **netvsc** (synthetic NIC over VMBus) on Hyper-V — both reuse infrastructure
  TinyOS already has (virtqueues from `vblk64`; VMBus from `vmbus64`/`storvsc64`).
- **USB** is a large, separate driver stack (host-controller + core +
  class drivers) whose payoff is mostly on **real hardware**; on a VM, networking
  should go through the paravirtual NIC, not USB. So USB is planned but deferred
  (Phase N5), and is the natural companion to a future bare-metal (real Intel PC)
  target.
- TCP/IP also composes with **Tier 1** (busybox): once sockets exist, `wget`,
  `nc`, `ping`, and a tiny `httpd` become real tests.

## Phased plan

### N1 — NIC driver (paravirtual)
**Why:** move Ethernet frames in/out.
**Tasks:** **virtio-net** (QEMU) first — RX/TX virtqueues (same split-virtqueue
pattern as `vblk64`), the net header, MAC/link, RX refill; then **netvsc**
(Hyper-V) over VMBus (reuse `vmbus64`: open a channel, RNDIS control + data
paths). Use QEMU **user networking (SLIRP)** so no host setup is required.
**Gate:** send a raw Ethernet/ARP frame and see the reply captured.

### N2 — TCP/IP stack
**Why:** turn frames into IP + TCP/UDP.
**Tasks:** **port lwIP** (recommended — small, MIT-licensed, embeddable, proven;
far less risk than writing TCP from scratch) behind a thin driver shim to N1;
bring up ARP, IPv4, ICMP (ping), UDP, TCP; a **DHCP client** for addressing and
a minimal **DNS** resolver. (Alternative: a hand-written minimal stack — more
educational, more bugs; lwIP is the pragmatic choice.)
**Gate:** `ping` an outside host and resolve a DNS name.

### N3 — Socket layer + Linux socket syscalls
**Why:** expose the stack to programs (busybox `wget`/`nc`, a server).
**Tasks:** BSD sockets mapped to lwIP: `socket (41)`, `bind (49)`, `connect (42)`,
`listen (50)`, `accept4 (288)`, `sendto (44)`/`recvfrom (45)`,
`sendmsg`/`recvmsg`, `setsockopt (54)`/`getsockopt (55)`, `getsockname`/
`getpeername`, plus `poll`/`select` integration (Tier 1 Phase G). File-descriptor
integration so sockets are ordinary fds (read/write/close, `O_NONBLOCK`).
**Gate:** a C program opens a TCP connection and exchanges bytes.

### N4 — Integration & verification
**Why:** prove end-to-end connectivity with real tools.
**Tasks:** `ping`, DNS lookup, an **HTTP GET** (`wget`), and a tiny listening
server (`nc -l` / a 20-line httpd) reachable from the host.
**Gate:** fetch a URL from inside TinyOS and serve a page to the host.

### N5 — USB (deferred; real-hardware track)
**Why:** input, storage, and networking on real hardware (and CDC-ECM as an
alternate net path).
**Tasks (large):** a host-controller driver (**xHCI** for USB3, or UHCI/EHCI for
legacy), the **USB core** (bus enumeration, descriptor parsing, control/bulk/
interrupt transfers, endpoints), then class drivers: **HID** (keyboard/mouse),
**mass storage** (BOT/UAS), and **CDC-ECM** (USB networking → feeds N2). Verify on
QEMU's emulated xHCI first, then real hardware.
**Gate:** a USB HID keyboard enumerates and delivers keystrokes; (stretch) a USB
mass-storage device mounts.

## Dependencies & out of scope

- **Depends on:** Tier 1 fd/process model and `poll`/`select` (Phase G) for a
  usable socket layer; the NIC drivers are independent and can start earlier.
- **Out of scope:** IPv6 (initially), TLS (huge — a later, optional port of a
  small TLS lib), Wi-Fi (needs USB/PCIe Wi-Fi + firmware), routing/NAT.
- **Decisions to make:** (a) **port lwIP** vs write a minimal stack from scratch?
  (b) QEMU virtio-net + SLIRP as the dev path, netvsc for Hyper-V parity?
  (c) is USB worth it before a real-hardware (bare-metal) target exists?

## Summary

| Phase | Unlocks | First target | Gate |
|---|---|---|---|
| N1 | Ethernet frames | virtio-net (QEMU) | ARP round-trip |
| N2 | IP/TCP/UDP | lwIP + DHCP/DNS | `ping` + DNS |
| N3 | sockets for apps | Linux socket syscalls | TCP byte exchange |
| N4 | real tools | wget / httpd | fetch a URL |
| N5 | real-HW USB | xHCI + HID | keyboard enumerates |
