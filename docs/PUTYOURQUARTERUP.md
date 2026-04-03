# putyourquarterup.com (PYQU)

## The Decentralized Arcade

Rebuild the arcade economy on the internet. Anyone with a GPU hosts MVC2.
Anyone with a browser plays. Every match costs a quarter. Winner takes the pot.
Hosts earn for running nodes. The FGC gets its arcade back.

---

## Core Architecture

```
                         ┌─────────────────────────┐
                         │    putyourquarterup.com  │
                         │    ─────────────────     │
                         │    Matchmaker + Lobby    │
                         │    Wallet / Ledger       │
                         │    NOBD Identity         │
                         │    Host Registry         │
                         │    Reputation Engine      │
                         └────────┬────────────────┘
                                  │
                    ┌─────────────┼─────────────┐
                    │             │              │
              ┌─────▼─────┐ ┌────▼────┐  ┌─────▼─────┐
              │  HOST NYC  │ │HOST ATL │  │ HOST LAX  │
              │  Dell+GPU  │ │ Gaming  │  │  Mini PC  │
              │  5ms E2E   │ │   PC    │  │  8ms E2E  │
              └─────┬──────┘ └────┬────┘  └─────┬─────┘
                    │             │              │
                WebRTC P2P    WebRTC P2P     WebRTC P2P
                    │             │              │
              ┌─────▼──┐   ┌─────▼──┐    ┌─────▼──┐
              │Player A │   │Player C│    │Player E│
              │(browser)│   │(browser│    │(browser│
              │Player B │   │Player D│    │Player F│
              └────────┘   └────────┘    └────────┘
```

### Components

1. **PYQU Web App** (putyourquarterup.com)
   - Matchmaking lobby — see who's online, challenge anyone
   - Crypto wallet integration (connect wallet or create custodial)
   - NOBD identity system — your tag, your record, your reputation
   - Host registry — map of all active nodes, their region, latency, rating
   - Match escrow — holds funds during match, pays out on result
   - Spectator system — watch live matches, bet on outcomes

2. **PYQU Host Client** (downloadable)
   - One-click installer for Windows/Linux
   - Bundles: Flycast + Maplecast streaming stack + H.264 encode + XDP input
   - Auto-configures for host's hardware (detects GPU, Quick Sync, etc.)
   - Connects to PYQU matchmaker on launch
   - Runs headless — no monitor needed, just internet + GPU
   - Auto-updates, health monitoring, earnings dashboard

3. **Player Client** (zero install — it's the browser)
   - WebRTC video player (already built)
   - Input via DataChannel (already built)
   - Wallet connect for payments
   - That's it. Click a link, play MVC2.

---

## The Token Economy: $QUARTER

Why crypto: no payment processor will touch real-money wagering on video games
without a fight. Crypto sidesteps Stripe/PayPal TOS, enables instant settlement,
and the metaphor is perfect — you're literally putting up a digital quarter.

### $QUARTER Token

- ERC-20 or Solana SPL token (Solana preferred for speed + low fees)
- 1 $QUARTER = ~$0.25 USD at launch (pegged or floating)
- Buy $QUARTER with crypto on-ramp (MoonPay, Ramp, etc.)
- Or earn $QUARTER by hosting

### Match Payment Flow

```
Player A wallet ──1 $QUARTER──►┌──────────────┐
                                │  Match Escrow │
Player B wallet ──1 $QUARTER──►│  (smart contract)
                                └──────┬───────┘
                                       │
                                  Match plays out
                                       │
                                ┌──────▼───────┐
                                │  Settlement   │
                                │               │
                                │  Winner: 1.40 $Q (70%)
                                │  Host:   0.40 $Q (20%)
                                │  PYQU:   0.20 $Q (10%)
                                └──────────────┘
```

### Money Match Tiers

| Tier              | Buy-in        | Winner Takes | Host Cut | PYQU Cut |
|-------------------|---------------|-------------|----------|----------|
| Quarter Match     | 1 $Q (~$0.25) | 1.40 $Q     | 0.40 $Q  | 0.20 $Q  |
| Dollar Match      | 4 $Q (~$1.00) | 5.60 $Q     | 1.60 $Q  | 0.80 $Q  |
| $5 Set            | 20 $Q         | 28.00 $Q    | 8.00 $Q  | 4.00 $Q  |
| $20 Money Match   | 80 $Q         | 112.00 $Q   | 32.00 $Q | 16.00 $Q |
| $100 Grudge Match | 400 $Q        | 560.00 $Q   | 160 $Q   | 80 $Q    |

### Free Play Mode

Not every match needs money. Casual lobby = free, no wallet needed.
This is important for onboarding. Let people taste it before they bet.

---

## Host Economics

### Why Host?

You have a PC. It's sitting there 16 hours a day doing nothing. Run the PYQU
host client and it prints money while you sleep.

### Earnings Model

| Metric                        | Value              |
|-------------------------------|--------------------|
| Average match length          | ~3 minutes         |
| Matches per hour              | ~20                |
| Host cut per quarter match    | 0.40 $Q (~$0.10)  |
| Hourly earnings (full util)   | ~$2.00             |
| Daily earnings (8hr active)   | ~$16.00            |
| Monthly earnings              | ~$480              |
| Electricity cost (Dell SFF)   | ~$5-10/mo          |
| Internet (already have it)    | $0                 |
| **Net monthly profit**        | **~$470**          |
| Hardware cost (used Dell)     | ~$60 one-time      |
| **Payback period**            | **~3 days**        |

### Host Tiers (NOBD Reputation)

Hosts earn reputation based on uptime, match completion rate, stream quality,
and player ratings. Higher tier = more traffic = more money.

| Tier       | Requirements                           | Perks                          |
|------------|----------------------------------------|--------------------------------|
| Bronze     | New host, < 100 matches                | Standard matchmaking           |
| Silver     | 100+ matches, 95%+ completion, 4+ stars| Priority in region             |
| Gold       | 1000+ matches, 98%+ completion         | Featured host, tournament eligible |
| Platinum   | 5000+ matches, verified hardware       | Money match host, highest payout |
| Arcade     | Physical location, multiple cabinets   | Listed as "Arcade", premium status |

---

## Crazy Ideas

### 1. Spectator Betting

Anyone can watch any match. While watching, you can bet on the outcome.

```
┌─────────────────────────────────┐
│  LIVE: SmoothViper vs KingCobra │
│  MVC2 — Quarter Match           │
│                                  │
│  [VIDEO STREAM]                  │
│                                  │
│  Spectators: 47                  │
│  Side bets pool: 230 $Q         │
│                                  │
│  ┌──────────┐  ┌──────────┐     │
│  │ Viper    │  │ Cobra    │     │
│  │ 1.8x     │  │ 2.2x     │     │
│  │ [BET 5Q] │  │ [BET 5Q] │     │
│  └──────────┘  └──────────┘     │
└─────────────────────────────────┘
```

Dynamic odds based on bet distribution. PYQU takes 5% of spectator pool.
This turns every match into a spectator event. People will watch MVC2 just
to gamble on it. This is literally what arcades were — people crowding around
watching money matches.

### 2. The Arcade Cabinet (Physical Node)

Sell a pre-built PYQU arcade cabinet:
- Raspberry Pi or N100 board inside (just needs to run the host client)
- Arcade stick + monitor built in
- Connects to PYQU network automatically
- Put it in a barbershop, laundromat, pizza shop, bodega
- The shop owner earns $QUARTER passively
- Players walk up, scan QR, connect wallet, play
- **It's literally an arcade cabinet that earns crypto**

Cost to build: ~$300-400 (screen + board + stick + case)
Earnings: same as any host node, but with local foot traffic too
The bodega owner doesn't need to know anything about crypto or emulators.
Plug it in, it works, money shows up.

### 3. Tournament Protocol

Built-in tournament brackets, fully on-chain:

- Anyone can create a tournament (8, 16, 32, 64 players)
- Entry fee in $QUARTER, prize pool is automatic
- Bracket is on-chain — no bracket manipulation, no TO drama
- Hosts are auto-assigned by region for lowest latency
- Stream auto-switches to active matches for spectators
- Grand finals auto-promoted to front page with spectator betting

**Weekly PYQU Majors:**
- Every Saturday, automatic 64-man MVC2 tournament
- $10 entry (40 $Q), $640 pot, winner takes $450
- Top 8 streamed with commentary (community can apply to commentate)
- These become the online majors the FGC never had

### 4. Replay NFTs

Every match is recorded. Iconic moments can be minted:

- Full combo that kills 3 characters? Mint it.
- Comeback from 1 pixel of health? Mint it.
- Community votes on "Moment of the Week" — auto-minted
- Replay NFTs include the actual replay data — re-watchable from any angle
- Trade them, collect them, flex them on your NOBD profile

Not a cash grab — a **highlight reel that you own**. The FGC already shares
clips endlessly. This just makes them collectible.

### 5. Regional Leaderboards + Turf Wars

Map-based regional competition:

```
┌──────────────────────────────────┐
│         PYQU TURF MAP            │
│                                   │
│   NYC ████████ (1,200 matches)   │
│   ATL ██████ (890 matches)       │
│   LAX █████████ (1,450 matches)  │
│   CHI ████ (560 matches)         │
│   HOU ███ (340 matches)          │
│                                   │
│   NYC vs LAX Turf War: LIVE      │
│   Score: NYC 47 — LAX 52         │
│   Prize pool: 10,000 $Q          │
└──────────────────────────────────┘
```

- Cities compete for weekly bragging rights
- Cross-region matches count toward turf score
- Winning city splits a bonus pool among its players
- This drives engagement, tribalism, content creation
- "NYC is free" becomes a bet-able statement

### 6. Commentary Marketplace

Community members can apply to commentate matches:

- Commentators connect audio to live matches
- Spectators tip commentators in $QUARTER
- Top commentators get auto-assigned to big matches
- Commentary audio is layered on the WebRTC stream
- Build the next Yipes from your bedroom

### 7. Training Mode Economy

Hosts can run training lobbies:

- Coaches offer paid training sessions (set their own rate in $Q)
- "Play against the best Storm player in NYC for 5 $Q per game"
- Coaching tools: slow-mo replay, input display, frame data overlay
- Creates a coaching economy — top players monetize their skill
- Players pay to level up, coaches earn, hosts earn from traffic

### 8. The PYQU Stick (Hardware Play)

Custom arcade stick with built-in NOBD identity:

- USB-C arcade stick, Sanwa parts
- Hardware secure element stores NOBD key
- Plug into any PYQU cabinet or PC, instantly logged in
- Your identity, your wallet, your record — in the stick
- Anti-cheat: hardware attestation that inputs come from a real stick
- Limited drops, community designs, FGC artist collabs
- $80-120 price point, the only arcade stick that earns you money

### 9. Host Staking

Hosts can stake $QUARTER to boost their node priority:

- Stake 1000 $Q → guaranteed top-tier matchmaking for 30 days
- If host maintains 99%+ uptime and quality, stake returned + bonus
- If host has downtime or bad quality, stake slashed
- Creates skin in the game — hosts who stake deliver better service
- Slashed tokens go to a community prize pool

### 10. Cross-Game Expansion

MVC2 is game one. The protocol works for any game Flycast supports:

- Third Strike (SF3)
- Capcom vs SNK 2
- Power Stone 2
- Guilty Gear XX
- Soul Calibur
- JoJo's Bizarre Adventure

Each game gets its own lobby, leaderboard, economy. But $QUARTER is universal.
Your NOBD identity carries across all games. Your reputation is portable.

Eventually, expand beyond Dreamcast:
- MAME for CPS2/CPS3 games (ST, Alpha 3, etc.)
- Naomi / Atomiswave (same Flycast core)
- Even N64 (Smash 64 community would go insane)

The protocol is game-agnostic. The streaming tech works for anything.
putyourquarterup.com becomes the decentralized arcade for ALL retro fighting games.

---

## Infrastructure Costs at Scale

| Scale         | Hosts | Matches/day | PYQU Server Cost | PYQU Revenue/mo |
|---------------|-------|-------------|------------------|-----------------|
| Launch        | 10    | 100         | $10/mo (1 VPS)   | $150            |
| Early growth  | 50    | 1,000       | $50/mo           | $1,500          |
| Fightcade-lvl | 200   | 10,000      | $200/mo          | $15,000         |
| Breakout      | 1,000 | 100,000     | $1,000/mo        | $150,000        |
| Money matches | —     | —           | —                | 3-5x multiplier |

Your costs are basically just the matchmaker and TURN relay servers.
ALL the GPU compute, ALL the bandwidth, ALL the encoding is donated by hosts
who are economically incentivized to provide it. The more popular the platform,
the more hosts join, the better the service gets. Flywheel.

---

## Tech Stack Summary

| Component           | Technology                              | Status      |
|---------------------|-----------------------------------------|-------------|
| Game emulation      | Flycast (Dreamcast)                     | BUILT       |
| Video encode        | H.264 hardware (GPU/Quick Sync)         | BUILT       |
| Streaming transport | WebRTC (video + DataChannel)            | BUILT       |
| Input pipeline      | AF_XDP zero-copy → kcode[] atomics      | BUILT       |
| Player client       | Browser (WASM viewer)                   | BUILT       |
| Matchmaker          | Node.js or Rust, WebSocket              | TODO        |
| NOBD identity       | Hardware UID + crypto wallet            | DESIGNED    |
| Token ($QUARTER)    | Solana SPL token                        | TODO        |
| Match escrow        | Solana smart contract (program)         | TODO        |
| Host client package | Electron or native installer            | TODO        |
| Spectator system    | WebRTC fan-out / SFU                    | TODO        |
| Reputation engine   | On-chain match history + ratings        | TODO        |
| Tournament protocol | On-chain brackets + auto-payout         | TODO        |

The hard part — real-time game streaming with sub-5ms input latency — is done.
Everything else is web app, smart contracts, and packaging.

---

## The Pitch (One Sentence)

**putyourquarterup.com is a decentralized arcade where anyone can host fighting
games on their PC, anyone can play in a browser, and every match costs a quarter —
rebuilt for the internet with crypto payments, community-hosted nodes, and the
lowest-latency game streaming stack ever built.**

Put your quarter up.
