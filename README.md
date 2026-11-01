# cengarde

**REAL UDP BONDING: every packet over every link.**

*A C engine that bonds several WAN links (4G/5G modems, fiber, Starlink) into
one fast, steady connection, per packet, not per flow. For OpenWrt routers and
a Linux VPS.*

cengarde carries your LAN's IP packets over all your links at once to a VPS,
through its own tunnel: a TUN interface (`cg0`) at each end and its own UDP
protocol (protocol 12). It began as a fork of
[engarde](https://github.com/porech/engarde) (Go) and is now a new engine,
with no WireGuard since 0.12. Both ends are always cengarde.

```
                          +== link 1 (5G)  ==+
LAN -- OpenWrt router ----+== link 2 (5G)  ==+---- VPS ---- Internet
        cg0 (cengarde)    +== link 3 (fiber)=+     cg0 (cengarde)
              each packet goes where it arrives first (bonding),
              or on every link (redundancy); the far end reorders
```

## Why per-packet bonding

| | Per-flow load balancing (multi-WAN routers) | MPTCP (OpenMPTCProuter) | cengarde |
| --- | --- | --- | --- |
| Unit of balancing | one connection stays on one link | TCP subflows per link | **every packet** |
| One TCP download uses all links | no | yes | yes |
| One UDP / RIST / QUIC stream uses all links | no | no (TCP only) | **yes** |
| Rides out a link cut | the connection breaks | TCP recovers | yes; in redundancy mode, without losing a packet |
| Loss protection (FEC, resend) | no | TCP retransmits only | FEC and a rescue ARQ |

How it works:

- Each packet goes on the link where it will arrive first (an ETA split), so a
  single connection can use the capacity of every link at once.
- Each link has its own subflow, with its own sequence numbers, its own loss
  detection (SFLOSS/SFACK) and its own rate: a BBR per link (`link_cc`).
- A reorderer at the far end puts the packets back in order.
- Classic multi-WAN routers balance per connection, and MPTCP only bonds TCP.
  cengarde bonds every packet, UDP included.

## Features

**Modes** (LuCI, or UCI `option mode`):

- **Bonding** adds the links up: the ETA split, GRIT, subflows and `link_cc`.
- **Redundancy** copies each packet over every link that delivers it within
  150 ms of the first copy, in proportion to each link's own rate. The first
  copy wins. It is for live broadcasts and for links that cut.
- **Custom** sets every knob by hand.

The VPS follows with `cengarde-vps-setup mode`, and one router can
administer the VPS over a restricted SSH key.

**GRIT, the protocontroller.** The router and the VPS split TCP: the LAN's
TCP ends at the router, crosses the links as GRIT (cengarde's own reliable
multipath stream, variant B) and the VPS opens a fresh TCP to the
destination. Variant A puts a plain kernel TCP in the middle.

**Protection:**

- FEC v3: sliding-window RLC (`fec_code = rlc`);
- a rescue ARQ: the receiver sends a NACK and the sender resends on the
  fastest link;
- copies at Starlink slot edges;
- per-link types (`cellular`, `starlink`, `fiber`, `ethernet`, `radio`), each
  with its own preset.

**Operation:**

- an OpenWrt package with a LuCI app (Spanish translation), and images for the
  Raspberry Pi 4 and x86-64;
- pairing with one shared secret;
- live status (RTT, delay, which link delivers first) in LuCI, as JSON or with
  `cengarde ctl`;
- reload without cutting, and pause or resume a link;
- IPv4 and IPv6 to the VPS, and optional IPv6 inside the tunnel;
- several routers per VPS;
- a VPS installer: `contrib/vps`, cloud-init and `cengarde-vps-setup`;
- authenticated protocol (SipHash-2-4, anti-replay window); the tunnel
  authenticates but does not encrypt, so use HTTPS, SSH or keyed SRT/RIST on
  top when you need secrecy.

## Field results

Testnet: an Intel NUC running OpenWrt with four 5G/LTE modems (Entel, WOM,
Movistar, Claro, in Chile) and a Vultr VPS in Santiago, against
OpenMPTCProuter (OMR) on the same modems. These are field measurements, not
`bench/` figures, and cells vary by hour, so read them as a snapshot.

| Run | cengarde | OMR |
| --- | --- | --- |
| Afternoon 2026-10-28 (`field-m1`, `field-m2`), 100 MB from cachefly + iperf3 upload: download | 142.7 and 146.7 Mbit/s | 101.8 and 115.9 |
| same, upload | 104.9 and 108.1 | 90.4 and 93.9 |
| same, loaded ping p95 | 47-60 ms | 79-448 ms |
| Night 2026-10-29 02:30, empty cells, Ookla CLI, 3 alternating rounds, FEC off: download (mean) | 454 / 529 / 678 (554) | 665 / 456 / 746 (622) |
| same, upload (mean) | 132 / 112 / 133 (126) | 113 / 51 / 146 (103) |
| same, loaded ping | 75-135 ms | 349-871 ms |

- Afternoon: download +27 to +40 %, upload +15 %, at half the ping or less.
- Night: download about level (OMR +12 %, within the round-to-round spread),
  upload +22 % for cengarde, loaded ping about a fifth of OMR's.
- Redundancy mode (2026-10-29, `field-r7`), GRIT against a plain TCP in the
  middle: download +9 % with lower ping and 15 % less data, upload +12 %.

### Evening of 2026-10-31: against OMR, UDP and a dead primary link

Same four modems, one router, and two Vultr VPSes in Santiago, one per
system. The arms always alternated, never at once. This is one evening on one
testnet, and a busy one: absolute numbers are far below the early-morning
tests of 2026-10-29 (both systems were above 500 Mbit/s then), so compare the
two columns, not the figures with the night above.

| Test | cengarde | OMR |
| --- | --- | --- |
| Speedtest (Ookla CLI, Santiago server, 3 rounds): download, mean | 184 Mbit/s | 194 Mbit/s |
| same, upload, mean | 119 Mbit/s | 94 Mbit/s |
| same, idle ping | 37-41 ms | 44 ms |
| same, ping loaded by the download | 54-62 ms | 624-798 ms |
| same, ping loaded by the upload | 78-91 ms | 128-167 ms |

- Download is level (OMR ahead in two rounds, cengarde in one, within the
  noise). Upload is +27 % on the mean, with cengarde ahead in two of the three
  rounds. The loaded ping during the download is about 12 times lower.
- **UDP at fixed rates** (iperf3, 10 s). OMR carries UDP through its VPN over
  a single link and receives no more than about 25 Mbit/s; downloading at 60,
  100 and 150 Mbit/s it lost 53, 77 and 86 %. cengarde spreads UDP over all
  the links: 0 % loss downloading at 150 Mbit/s. Uploading it carried up to
  about 80 Mbit/s (the modems' total upload is about 120); at 150 Mbit/s it
  sent more than they carry and lost 53 %.
- **The primary link dies with its interface still up** (Entel, OMR's MPTCP
  master; all its traffic dropped), in the middle of a download:

| Cut | cengarde | OMR |
| --- | --- | --- |
| 4 TCP flows, 25 s cut, 2 rounds: seconds near zero | 0 and 1 | 8 and 1 |
| same, ping loss | 0 % in both rounds | 32 % in round 1, 0 % in round 2 |
| 1 TCP flow of 90 s, 45 s cut, 2 rounds: connection survives | yes, both | yes, both |
| same, seconds near zero | 0 and 1 (about 190 to 150 Mbit/s on three modems) | 14 and 5 |
| New HTTPS connection every 2 s during the cut: succeeded | 46 of 46 | 20 of 27 |
| same, median time to connect | 0.23-0.26 s | 1.7-1.8 s (worst 3.4 s) |

  Each MPTCP connection opens its first subflow on the master, so OMR's new
  connections wait on the dead link. cengarde has no master link.
- **Fairness caveat:** the iperf3 target of the UDP and cut tests was the
  cengarde VPS, which OMR reached over the internet, one hop inside the same
  datacenter. The new-connection test went to a neutral server. cengarde's
  UDP was measured through its tunnel address.
- **Engine changes behind these numbers** (this week, A/B of 3 alternating
  rounds per arm, in the field):
  - a wider per-link subflow memory, so lost packets are always resent
    promptly: download +29 % (223 to 289 Mbit/s, PR #136);
  - the stress copies are off in bonding mode, because they flooded slow
    links (PR #137);
  - GRIT's retransmission timer now knows when each packet's link should
    deliver it (MPGRIT step U2a): download +17 % and 35 % fewer duplicate
    retransmissions (PR #139, open).

## Quick start

### Router (OpenWrt, recommended)

1. **Install:** flash the image (Pi 4 or x86-64) or install the packages
   (`cengarde`, `luci-app-cengarde`) on your OpenWrt.
2. **Create the links:** one interface per link (DHCP on each VLAN or modem).
3. **Create the VPS:** in LuCI, **Services -> cengarde -> VPS** gives you a
   ready cloud-config to paste as *user data* when you create the VPS.
4. **Pair and enable:** put the VPS address (or several, IPv4 and IPv6) and
   the one shared secret, pick the links and enable.
5. **Check:** open the **Status** page.

The full guide, with images and packages, is in
[`openwrt/README.md`](openwrt/README.md).

### VPS

Either paste the cloud-config at creation, or on an existing Linux VPS run
`sudo sh contrib/vps/install.sh`, then use `cengarde-vps-setup` to follow the
mode or add routers and port forwards. See [`contrib/vps/`](contrib/vps/).

### Another Linux, by hand

```sh
make -C engine && sudo make -C engine install   # /usr/sbin/cengarde
cengarde genkey                                  # the shared secret
```

Client and server configurations are explained in
[`engine/README.md`](engine/README.md).

## Status and roadmap

- Latest release: v0.13.0; 0.13.1 is coming (a fix for GRIT's shared memory
  pool).
- Done: the engine, bonding and redundancy modes, GRIT, FEC v3, the rescue
  ARQ, link types, IPv6, several routers per VPS, names with dynamic DNS, and
  OpenWrt with LuCI and images, all tested end to end in QEMU.
- Missing: more time on real hardware beyond the testnet, a signed OpenWrt
  feed, static binaries, Debian and Raspberry Pi OS packages, and optional
  eBPF/XDP on the VPS.
- Compatibility: both ends must be cengarde at the same protocol version; it
  does not talk to engarde.

The plan is in [`ROADMAP.md`](ROADMAP.md).

## Repository layout

| Folder | What is in it |
| --- | --- |
| [`engine/`](engine/) | the C engine: sources, tests and config examples |
| [`openwrt/`](openwrt/) | the `cengarde` and `luci-app-cengarde` packages, the guide and the QEMU tests |
| [`contrib/`](contrib/) | the VPS: cloud-config, NAT and the systemd units |
| [`bench/`](bench/) | the measurement lab (network namespaces, netem) |
| [`docs/`](docs/) | research stories (`historias/`) and the engine map (`mapa/`) |

## Development

```sh
make -C engine test                                    # unit tests
make -C engine SANITIZE=1 test                         # with ASan and UBSan
sudo bench/lab.sh build && sudo bench/lab.sh ci        # the lab
openwrt/test/e2e.sh <x86-64-image>                     # OpenWrt end to end, in QEMU
```

CI builds with gcc and clang (`-Werror`), runs ASan and TSAN, tests on
aarch64, armhf and MIPS big-endian, runs the lab in netns, builds the OpenWrt
packages and images, runs the end-to-end test, and publishes a release on a
`v*` tag.

The detailed docs and the research stories in `docs/historias/` are in
Spanish.

## Credits and license

cengarde began as a fork of [engarde](https://github.com/porech/engarde) by
Alessandro Rinaldi (porech): the idea of sending packets over every link, and
years of real use broadcasting live radio from the field. The original Go
code remains in the git history (commit `3492df9`). The link-health ideas
(muting, weighted round robin, ARQ) come from libRIST, and the Reed-Solomon
FEC vendors [Leopard-RS](https://github.com/catid/leopard).

License: GPL-3.0-or-later, see [LICENSE.txt](LICENSE.txt). Until the license
change cengarde was published under GPL-2.0, like engarde. Nothing of
engarde's code is left in the tree; the git history keeps its commits under
their original license.
