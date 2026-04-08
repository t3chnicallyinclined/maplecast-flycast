# Security Policy

## Reporting a Vulnerability

If you find a security issue in MapleCast — anything that could let someone gain unauthorized access to a running instance, leak player data, crash the server, or interfere with another player's match — **please don't open a public GitHub issue**. Public issues are great for bugs but bad for vulnerabilities, because they give attackers a window between disclosure and patch.

Instead, report it privately:

- **Email:** [t3chincallyinclined@proton.me](mailto:t3chincallyinclined@proton.me)
- **Subject line:** start with `[security]` so it doesn't get lost
- **Include:** what you found, how to reproduce it, and what you think the impact is. A working PoC is appreciated but not required.

You'll get an acknowledgement within a few days. If the issue is real, expect a fix and a coordinated disclosure on a timeline that matches the severity — usually within a couple of weeks for high-impact issues, faster for active exploitation.

## Scope

**In scope:**

- The Flycast-derived server code (`core/`, especially anything under `core/network/maplecast_*`)
- The Rust spectator relay (`relay/`)
- The Rust stats collector (`web/collector/`)
- The browser client (`web/king.html`, `web/js/*`, `packages/renderer/`)
- The build/deploy tooling under `deploy/`

**Out of scope:**

- Issues in upstream Flycast that aren't introduced by MapleCast modifications — please report those to the [upstream Flycast project](https://github.com/flyinghead/flycast/issues) instead
- ROM piracy, cheating in-game, or anything that requires the attacker to already be a legitimate player
- Bugs in third-party dependencies (`tokio`, `websocketpp`, `nlohmann/json`, etc.) — report those upstream
- Vulnerabilities in deployments other than the canonical [nobd.net](https://nobd.net) instance — if you operate your own MapleCast deployment, you own its security posture

## What counts as a vulnerability

Things we care about, in rough order of severity:

1. **Remote code execution** on the server or any client browser
2. **Authentication bypass** — joining a slot without going through the queue, taking over another player's slot, accessing admin endpoints without an admin role
3. **Determinism breaks** that let one client desync the whole stream for everyone (the wire is byte-perfect deterministic by design — anything that lets a client disrupt that is a real bug)
4. **Denial of service** — anything that lets a single client crash the headless server or kick legitimate players
5. **Information disclosure** — leaking another player's stats, identity, or stick binding to an unauthorized party

Things we *don't* consider vulnerabilities:

- The diagnostics modal exposes per-player input timing and frame phase. That's intentional — it's a feature, not a leak.
- The relay broadcasts the same TA frames to every connected spectator. That's the entire point.
- Any "issue" that boils down to "MapleCast lets people play MVC2 in a browser without owning the ROM" — that's not a security bug, that's not a MapleCast concern, and ROM ownership is the operator's responsibility.

## Known caveats

- The spectator broadcast is **public by design**. Anyone who can reach the relay's WebSocket can watch the stream. There is no spectator authentication and we don't intend to add one.
- The `nobd.net` production instance runs an admin panel at `/overlord` whose authentication is documented in private operator docs. The endpoint exists publicly; the auth is real (argon2 + scoped tokens), but the panel is not in scope for public security disclosures because it's operator-managed infrastructure.

## Hall of fame

Security researchers who responsibly disclose issues will be credited here (with their permission) once the fix is shipped.

*(empty for now — be the first)*
