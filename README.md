# PalCrosschat

UE4SS C++ mod for Palworld dedicated servers. Relays in-game global chat to a shared MySQL database and injects messages from other servers (or a Discord bot) into local chat.

Two servers typically run this mod with different configs: **pal-na** and **pal-eu**.

## Requirements

- Windows x64 Palworld dedicated server (`PalServer-Win64-Shipping-Cmd.exe`)
- **Okaetsu experimental-palworld** UE4SS build (with `MemberVariableLayout.ini`). Stock UE4SS crashes on current Palworld.
- Visual Studio 2022 (MSVC toolset meeting current RE-UE4SS requirements; C++23)
- CMake >= 3.22
- Rust toolchain (needed to build RE-UE4SS from source)
- Ninja or the Visual Studio CMake generator
- A MySQL/MariaDB database with the existing crosschat schema (from the Discord bot project)

## Build (clean machine)

1. Install Visual Studio 2022 or newer with Desktop development with C++ (MSVC toolset meeting current RE-UE4SS requirements; C++23).
2. Install [CMake](https://cmake.org/download/) (>= 3.22) and ensure it is on `PATH`.
3. Install Rust (`rustup`) to build RE-UE4SS from source.
4. Clone or place [RE-UE4SS](https://github.com/UE4SS-RE/RE-UE4SS) at `C:\Repos\RE-UE4SS` (or pass `-DUE4SS_ROOT=`).
5. Clone this repo to `C:\Repos\PalCrosschat`.
6. Open a VS x64 Native Tools / Developer command prompt (or run `vcvars64.bat`), then configure and build **Game__Shipping__Win64** (must match your UE4SS binary CRT/config).

Ninja (recommended when the VS CMake generator does not detect your IDE install):

```bat
cd C:\Repos\PalCrosschat
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Game__Shipping__Win64 -DUE4SS_ROOT=C:/Repos/RE-UE4SS .
cmake --build build
```

Or Visual Studio generator (VS 2022):

```bat
cmake -B build -G "Visual Studio 17 2022" -A x64 -DUE4SS_ROOT=C:/Repos/RE-UE4SS .
cmake --build build --config Game__Shipping__Win64
```

A helper script `build.bat` is included for VS 2026 Developer Command Prompt + Ninja.

MariaDB Connector/C is downloaded and built automatically via CMake `ExternalProject` (static `mariadbclient`, no separate MySQL client install required).

Output package:

```
dist/PalCrosschat/
  dlls/main.dll
  enabled.txt
  config.json.example
```

Optional live install during build:

```bat
cmake -B build -G "Visual Studio 17 2022" -A x64 ^
  -DUE4SS_ROOT=C:/Repos/RE-UE4SS ^
  -DPALSERVER_MODS_DIR="C:/Path/To/PalServer/Binaries/Win64/ue4ss/Mods" .
cmake --build build --config Game__Shipping__Win64
```

## Install on the server

1. Confirm UE4SS is the Palworld-specific Okaetsu experimental build under `PalServer\Binaries\Win64\` (often `ue4ss\` layout).
2. Copy the package folder to:

```
...\Binaries\Win64\ue4ss\Mods\PalCrosschat\
  dlls\main.dll
  enabled.txt
  config.json          (you create this)
```

3. Copy `config.json.example` to `config.json` next to `enabled.txt` (mod root, **not** inside `dlls\`).
4. Edit `config.json` (see table below). Set `ServerOrigin` to `pal-na` or `pal-eu`.
5. Restart the dedicated server. Check the UE4SS log for `[PalCrosschat]` lines.

## Config reference

| Key | Section | Default | Notes |
|-----|---------|---------|-------|
| ServerOrigin | General | (required) | `pal-na` or `pal-eu`. Also used as DB consumer name. |
| PollIntervalMs | General | 750 | How often the DB worker polls for inbound rows. |
| MaxBatch | General | 50 | Max rows per poll SELECT. |
| MaxBroadcastsPerTick | General | 5 | Max injected chat lines per game tick. |
| DebugVerbose | General | false | Log every relayed message. |
| Host | MySQL | (required) | MySQL/MariaDB host. |
| Port | MySQL | 3306 | |
| User | MySQL | (required) | |
| Password | MySQL | | Never logged. |
| Database | MySQL | (required) | |
| ReconnectBackoffMaxSec | MySQL | 30 | Cap for exponential reconnect backoff. |
| PrefixNA | Format | `NA` | Value for `{prefix}` when origin is pal-na. |
| PrefixEU | Format | `EU` | Value for `{prefix}` when origin is pal-eu. |
| PrefixDiscord | Format | `Discord` | Value for `{prefix}` when origin is discord. |
| ChatFormat | Format | `[{prefix}] [{guild}] {player}: {message}` | In-game crosschat line template. Placeholders: `{prefix}`, `{guild}`, `{player}`, `{message}`. Empty guild removes stray `[]`. Split into Sender + Message for Palworld’s `[Sender]:Message` UI (expect a `]` before `:` — same as native `[Name]: text`). |
| InjectCategory | Format | `global` | `global` (1) or `discord` (4). |
| ShowLocalServerTag | Format | `false` | When `true`, local Global chat is cleared and rebroadcast using `ChatFormat` (same look as cross-server). |
| PreserveSenderUId | Format | `false` | Legacy (v1.78). Unused by dual broadcast (v1.79+). Kept for config compatibility. |
| LogWebhookUrl | ChatFilter | `""` | Optional Discord webhook URL for mute/block logs. Empty disables. |
| InitialMuteNotification | ChatFilter | `You have been muted for {mutetime}!` | Banner title line on first mute. Placeholders: `{mutetime}`, `{mutemessage}`, `{remainingtime}`. |
| ActiveMuteNotification | ChatFilter | `You are muted! Time remaining: {remainingtime}` | Banner title when a muted player chats again (no MuteMessage). Placeholder: `{remainingtime}`. |
| FilteredPatterns | ChatFilter | `[]` | Array of `{ Pattern, MuteMinutes, MuteMessage }`. Each `Pattern` is a case-insensitive ECMAScript regex. |

If `config.json` is missing, MySQL fields are empty, or `ServerOrigin` is invalid, the mod logs an error and disables itself. The game server keeps running.

### ChatFilter

On a local hit the message is cleared in `EnterChat_Receive` (never broadcasts or relays). The player is muted for that pattern’s `MuteMinutes` (`0` = block only). A red **Notifications from the server** banner is shown via `BroadcastServerNotice`:

- Initial: `{InitialMuteNotification}` plus the pattern’s `MuteMessage` on the next line  
- Active (chat while muted): `{ActiveMuteNotification}` only (throttled every 15s)

The client hardcodes the header text “Notifications from the server”; only the notice body is controllable. `BroadcastServerNotice` is NetMulticast (all clients can see the banner). A private `SendScreenLogToClient` line is also sent to the muted player.

Legacy `WordBlacklist` / `BlockedWords` configs still load if `ChatFilter` is absent.

Cross-server and Discord messages are filtered again on inject (`BroadcastChatMessage`). Inbound hits are dropped only (no mute of remote senders).

## Database contract

Tables already exist (do not create them from this mod):

- `crosschat_messages(id, origin, sender_name, sender_id, guild_name, message, created_at)`
- `crosschat_cursors(consumer, last_id)`
- `crosschat_players(DiscordId, Platform, UserId, PlatformUserId, PlayerName, LinkedAt, ConnectCode)` — Discord linking

This mod INSERTs with `origin = ServerOrigin` (including the player's Palworld guild name) and SELECTs `WHERE id > ? AND origin != ?`. Consumer name equals `ServerOrigin`. On first install with no cursor row, the cursor is set to `MAX(id)` so history is never replayed.

### Chat channels

Only **Global** chat is relayed: every `crosschat_messages` row is forwarded to the other
servers and Discord, so global is the only channel ever written there.

- **Global** — with `ShowLocalServerTag: true`, suppressed and rebroadcast with
  `ChatFormat`, visible to everyone; relayed to the DB.
- **Guild** — completely untouched: native green display, native audience, never leaves
  the server. (Reformatting guild chat is unworkable: clients discard Category=Guild
  lines whose `SenderPlayerUId` is nil, and a nil uid is required for a custom Sender
  string to render — see the v1.67–1.70 history.)
- **Say** — completely untouched and never leaves the server.

With `DebugVerbose`, each chat line logs `CHAT pal_cat=… relayed=…`, which is the
quickest way to confirm what channel the server actually saw for a given message.

### Xbox / console dual broadcast (v1.79+, v1.83)

Custom `ChatFormat` Sender tags require a nil `SenderPlayerUId`; Xbox clients mask
chat bodies they cannot attribute. Injected/rebroadcast lines split via
`ReceiverPlayerUIds`:

- **steam_ / ps5_ / unknown** — formatted Sender tags (`[EU][WOWZERS][Name]: hello`, nil uid).
- **gdk_ only** — plain native chat (`[Name]: hello`) with a **local** sender
  `PlayerUId` (no `[NA]` / guild tags). Platforms are warmed via SEH UniqueNetId
  when `AccountName` is empty.

Use a complete ChatFormat (note the `]` after `{player}`):
`"[{prefix}][{guild}][{player}]: {message}"`.

Remote/cross-server UIDs are never set as `SenderPlayerUId` (avoids `------`).
Discord → game rows have no Palworld `PlayerUId`, so Xbox may still mask those.

`ChatFormat` is split on the `{message}` placeholder before substitution, so
trailing template junk cannot leave the chat text in the Sender field and double it.

### Command handling

Chat lines starting with `/` or `!` (any chat mode) are hidden from chat and
**never written to the DB** — so commands (including `/adminlogin` and its
password) never show anywhere and never reach Discord or other servers.

Mechanism (v1.77): the pre-hook rewrites the `Category` byte to `None` (safe
in-place byte write) and leaves the Message text **intact**. Clients don't display
None-category chat, while PalDefender — which reads the command from the same
`EnterChat_Receive` parameters after this mod's pre-hook — still receives and
executes real commands (verified: `/adminlogin` works while parked). Two hard-won
rules encoded here:

- Never truncate a command line's Message: PalDefender gets an empty string and
  real commands stop executing (v1.73/v1.75).
- Never rebroadcast a command line: PalDefender does not empty consumed commands,
  so real and fake commands are indistinguishable, and v1.76 ended up formatting
  `/adminlogin <password>` into public chat.

A mistyped ("fake") command therefore just vanishes silently. The only command
parsed by this mod is its own `!setdiscord` (suppressed + private reply). The bot
likewise never relays `/`- or `!`-prefixed Discord messages into the game.

### Discord `!setdiscord`

When a player types `!setdiscord CODE` in chat (`!` avoids PalDefender `/` admin handling):

1. The command is **not relayed to MySQL** and the chat line is suppressed by truncating
   `Message` in place (never by reassigning the `FString` — that mismatched the game's
   allocator and corrupted the heap). Only the private PalCrosschat reply is visible.
2. The mod resolves the platform user id (`steam_…` / `gdk_…` / `ps5_…`) and caches it.
3. It looks up `ConnectCode` on `crosschat_players` and completes the link (or reports already linked / invalid code).
4. A private chat line reports the result to that player only.

Codes are created by the PalCrosschatBot **Link Discord** panel button.

If your DB still has the `category` column (added for v1.66, removed in v1.72 — only
global chat is written to the table now, so the label is dead weight), drop it:

```sql
DELETE FROM crosschat_messages WHERE category <> 0;
ALTER TABLE crosschat_messages
  DROP KEY idx_category_id,
  DROP COLUMN category;
```

If you already have the tables without `guild_name`, run:

```sql
ALTER TABLE crosschat_messages
  ADD COLUMN guild_name VARCHAR(64) NOT NULL DEFAULT '' AFTER sender_id;
```

If a table or column is missing, the mod logs that the schema is out of date, tells you to run `schema.sql` from the bot project, and keeps retrying on the backoff schedule.

## Testing without the bot

Insert a fake row and expect it in game within about two poll intervals:

```sql
INSERT INTO crosschat_messages (origin, sender_name, guild_name, message)
VALUES ('discord', 'TestUser', '', 'hello from the database');
```

Type a global chat message in game and confirm a row with `origin = ServerOrigin` appears within about one second under normal load.

Injected messages use `BroadcastChatMessage` (not `EnterChat_Receive`), so they are not re-captured. Watching the DB while injecting should not create duplicate rows from this server. The prefix filter is a second safety layer.

## InjectCategory experiment

Default is `global`. Palworld also defines a native Discord chat category (`4`). Admins should test `InjectCategory=discord` once to see how the client renders it. If it looks good, Discord-origin messages may look better with `InjectCategory=discord` and `PrefixDiscord` set to empty.

## After a Palworld update

If chat capture or injection stops working after a game patch, re-verify first:

1. `CAPTURE_HOOK_PATH` and `BROADCAST_FUNC_PATH` in `src/PalChatApi.h`
2. The `FPalChatMessage` field set documented in that same header

Re-check against a fresh [PalworldModdingKit](https://github.com/) clone / header dump, then confirm the capture hook still fires with a Lua probe:

```lua
RegisterHook("/Script/Pal.PalPlayerController:EnterChat_Receive", function(self, Message, Category)
    print("[Probe] chat fired\n")
end)
```

Rebuild this mod against the same UE4SS binary you ship on the server (CRT/ABI match).

## Threading model

- **Game thread:** hook callbacks and `on_update` only. Copy data into plain structs, push/pop queues, call `BroadcastChatMessage`. No MySQL, no blocking waits.
- **DB worker (`std::jthread`):** owns the MySQL connection. INSERT outbound, poll inbound, update cursor, reconnect with exponential backoff.
- Queues are mutex-protected, max 500 entries each. Full queues drop the oldest entry and warn.

## Acceptance checklist

- [ ] Builds to a single `main.dll` under `dist/PalCrosschat/dlls/`
- [ ] Server runs normally with MySQL unreachable; mod recovers when MySQL returns
- [ ] Chat typed in game appears as a DB row within ~1s
- [ ] Row with a different origin appears in game within two poll intervals
- [ ] Injected messages are not re-captured into the DB
- [ ] Server restart does not replay old messages (cursor persists)
- [ ] Exceptions in hooks/tick are logged and never crash the process
