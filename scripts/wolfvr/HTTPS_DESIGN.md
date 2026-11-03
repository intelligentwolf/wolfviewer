# WolfViewer VR in the headset browser: HTTPS with no warning (design, NOT built)

Status, 2026-11-03: this is a design for Paul to approve. No DNS has been changed, no certificate issued and no
service deployed. Stage 1 ships the fallbacks below, which need nothing on our servers.

## Why HTTPS at all

WebXR (`navigator.xr`) only exists in a *secure context*: an https:// page, or http://localhost. A page on
`http://192.168.1.9:4333` has no VR button. So the headset page must be one of:

| Way | Warning? | Set-up on the headset | Our servers |
|---|---|---|---|
| A. Self-signed certificate made on the PC (stage 1, built) | Once per PC ("not private", then Advanced > Proceed) | none | none |
| B. `adb reverse` over USB, `http://localhost:4333` (built) | none | developer mode, USB cable, adb on the PC | none |
| C. Quest Browser `chrome://flags` "Insecure origins treated as secure" for `http://<pc-ip>:4333` | none after set-up | one flag, browser relaunch | none |
| D. Plex-style real certificate per install (this design) | none | none | a DNS zone + a tiny issuer |

Evidence:
- **A works in Chromium.** Headless Chromium 2026-11-03, our server and our certificate: before accepting it was the
  "Privacy error" page. After Advanced > Proceed it showed `isSecureContext: true`, `typeof navigator.xr: "object"`
  and `XRSession` defined. The page's `wss://` socket to the same host:port connected, and the H.264 stream played.
  All of that still held after a reload (`scripts/wolfvr/` certtest, see the report). For the Quest itself, NVIDIA's
  CloudXR.js client guide documents the same flow on the headset browser: go to the address, choose Advanced, then
  Proceed. It says the trust "persists across browser sessions" and you "only need to perform this configuration once
  per certificate". https://docs.nvidia.com/cloudxr-sdk/latest/usr_guide/cloudxr_js/client_setup.html
  WolfViewer keeps its certificate (user_settings/wolfvr_cert.pem; the key is mode 0600), so the question comes once
  per PC address. Not yet seen on a real Quest; that is Paul's test.
- **B:** `http://localhost` is a secure context by specification. The emulator test ran over
  `http://localhost:4333` (`isSecureContext: true`, VR session entered). `adb reverse tcp:4333 tcp:4333` maps the
  headset's localhost to the PC's. It needs Meta developer mode, which Paul does not want ("don't want to mess around").
- **C** is documented by the same NVIDIA guide for Quest (HTTP mode). It needs one trip to `chrome://flags`, and you
  must type the PC's address exactly.

## How Plex does it (D)

Filippo Valsorda, "How Plex is doing HTTPS for all its users"
(https://words.filippo.io/how-plex-is-doing-https-for-all-its-users/):
- Plex runs a DNS zone, `plex.direct`, that answers `1-2-3-4.<hash>.plex.direct` with 1.2.3.4. The address is
  carried in the name, so no record is ever written per server.
- Each Plex Media Server gets its own wildcard certificate `*.<hash>.plex.direct` from DigiCert, through an
  intermediate DigiCert minted for Plex. Because the hash belongs to one server, that certificate identifies only that
  server whatever IP it is on, which is why handing the private key to the user is safe. There is no shared key.
- Known cost: routers and resolvers with DNS-rebinding protection refuse public names that answer with private
  addresses. Plex's documentation tells users to whitelist `plex.direct` (the Pi-hole and Plex forum threads are in
  the report sources).
- `plex.direct` is NOT on the Public Suffix List (checked against publicsuffix.org on 2026-11-03). Plex can do this
  because DigiCert is a commercial CA with no public per-domain issuance cap.

**Shipping one wildcard private key inside every viewer is ruled out.** Anyone could extract it, the CA would have to
revoke it as compromised (baseline requirements), and it would stop working for everyone at once.

## Proposed design for us

Names: `https://192-168-1-9.<id>.<zone>:4333/` (no code: since 2026-11-03 a headset is let in by Allow / Deny on
the PC, wolfvrpairing.h), where `<id>` is a random per-install label
(13 base32 characters, 64 bits) and `<zone>` is a domain kept for this alone (see "Which domain").

1. **The viewer** (when VR in Your Headset Browser is first switched on):
   - makes an EC P-256 key and a CSR for `*.<id>.<zone>`. The key stays in user_settings, mode 0600, and never
     leaves the PC;
   - sends the CSR in one HTTPS request to the issuer, with the Wolf login proof we already use (X-Wolf-Agent +
     X-Wolf-Session, checked against the grid presence service, as `viewerdob.php` does);
   - gets back the certificate chain and renews it from day 60 of 90. Under ACME ARI, renewals do not count against
     any Let's Encrypt limit;
   - falls back to A (self-signed) and shows both addresses whenever the name does not work (see "Failure modes").

   This one request every ~60 days is the only traffic that touches our servers. The headset's DNS lookup goes to
   its own resolver and then to our DNS servers, and the answer is a LAN address. **Video and poses never leave the
   LAN.**
2. **The issuer** (one small endpoint, PHP or Rust, on the existing web hosts):
   - checks the Wolf session, then rate-limits per account (for example 3 new ids per day and 10 in total);
   - assigns `<id>` (or reuses the account's id for that install);
   - checks that the CSR asks for exactly `*.<id>.<zone>` and nothing else;
   - runs ACME DNS-01 with Let's Encrypt, writing `_acme-challenge.<id>.<zone>` TXT into its own DNS;
   - returns the chain, and logs account, id and time (A09). It never sees a private key.
3. **The DNS zone** (an authoritative responder on two existing hosts, UDP and TCP port 53):
   - `a-b-c-d.<id>.<zone>` gets A a.b.c.d **only if** a.b.c.d is RFC 1918 (10/8, 172.16/12, 192.168/16). Anything
     else gets NXDOMAIN, so the zone can never point at a public machine;
   - serves `_acme-challenge` TXT from the issuer's store;
   - TTL 300 (the IP is in the name, so it never needs to change).

   PowerDNS with a pipe/remote backend, or a ~150-line responder, will do. Off-the-shelf "dynamic" DNS hosts
   (Cloudflare and the like) cannot work out an answer from the name.

### Which domain

Do NOT put it under `wolf-grid.com`. Let's Encrypt allows **50 new certificates per registered domain every 7 days**,
shared by every account. A registered domain is worked out with the Public Suffix List, so `vr.wolf-grid.com` would
share wolf-grid.com's 50 with all our real web certificates, and new VR installs would starve them
(https://letsencrypt.org/docs/rate-limits/). Instead:
- register a separate domain just for this, for example `wolfvr.net` (Paul's choice, about £10-15 a year);
- at 50 new installs a week, ask Let's Encrypt for a rate-limit increase (their form exists for exactly this kind of
  integration and takes a few weeks), and/or apply for the zone to go on the Public Suffix List, private section.
  Each `<id>.<zone>` then becomes its own registered domain.

### Cost

- Domain: about £10-15 a year.
- Let's Encrypt: free.
- DNS responder and issuer on existing WolfStack hosts: £0 in hosting.
- Work: about 1 day for the responder, 1-2 days for the issuer, 1 day for the viewer side (CSR, renewal, fallback).

### Security notes

- **Per-install keys:** stealing one key exposes only that install's names. Revocation is per install (the issuer
  keeps id to account).
- **Certificate Transparency** will publish every `*.<id>.<zone>`. The ids are random: no names, no IPs.
- **DNS rebinding:** a page from another site cannot use our zone to reach a LAN device inside its own origin. Our
  answers are fixed (no switching), and `<id>.<zone>` is a different origin from the attacker's site. Anyone can
  already do the same with nip.io or sslip.io, so we add no new capability. The headset server also refuses any page
  picture or input to a headset the PC has not allowed (Allow / Deny, wolfvrpairing.h), and the WebSocket checks
  Origin.
- **Abuse:** answering private addresses only means the zone cannot be used for phishing on public IPs. The issuance
  rate limits stop people farming certificates.
- **Nothing on the LAN listens beyond what stage 1 already does:** HTTPS on the LAN address only (never 0.0.0.0),
  plain HTTP on 127.0.0.1 for USB, TLS 1.2 or newer, plain HTTP on the LAN port only redirects to HTTPS, each
  headset allowed once on the PC (a 256-bit secret in an HttpOnly cookie, only its hash kept), one headset at a time,
  at most 16 connections.

### Failure modes (why the self-signed fallback stays)

- **Router or resolver DNS-rebinding protection** blocks private answers. Plex users hit this. FRITZ!Box, OpenWrt
  dnsmasq `stop-dns-rebind`, pfSense/OPNsense and optionally Pi-hole all do it. The name then fails to resolve, so
  the floater keeps showing the self-signed address as well.
- **Quest "Private DNS" / DoH** set to a filtering provider: the same symptom.
- **No internet:** there is no DNS answer, so the same fallback applies.

## What Paul needs to approve

1. The domain: buy a separate one (name of your choice), or accept the rate-limit risk on `vr.wolf-grid.com`.
2. Two hosts to run the authoritative DNS responder (public port 53 UDP and TCP), and the NS delegation.
3. The issuer endpoint on the web hosts, with a Let's Encrypt ACME account.
4. Then the viewer side (CSR, renewal, fallback), as a later stage.

Until then, stage 1 uses A (once per PC), with B for anyone who already has developer mode.
