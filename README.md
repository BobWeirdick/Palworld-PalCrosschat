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

### Discord `/setdiscord`

When a player types `/setdiscord CODE` in chat:

1. The command is suppressed (not relayed to MySQL chat).
2. The mod reads `APalPlayerState::AccountName` and normalizes to `steam_…` / `gdk_…` / `ps5_…`.
3. It looks up `ConnectCode` on `crosschat_players` and completes the link (`Platform`, `UserId`, `PlatformUserId`, `PlayerName`, `LinkedAt`, clears `ConnectCode`).
4. A server notice reports success or failure.

Codes are created by the CrosschatBot **Link Discord** panel button.

If you already have the tables without `guild_name`, run:

```sql
ALTER TABLE crosschat_messages
  ADD COLUMN guild_name VARCHAR(64) NOT NULL DEFAULT '' AFTER sender_id;
```

If tables are missing, the mod logs: schema not found, run `schema.sql` from the bot project, and keeps retrying on the backoff schedule.

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
