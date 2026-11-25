# HSMono

This Windows x64 development target loads a trusted managed smoke assembly,
passes it the local spawn ID captured on Hailstone's client thread, and
unloads its app domain when the session stops. The Mono root runtime stays
resident for the life of the process so the native plugin can reload and
create a fresh app domain. The smoke assembly has no native imports,
background worker, or game actions.

## Build

From this checkout's root (the folder holding `hailstone/` and `Mono/`), in a
Windows shell with Visual Studio 2022 and the .NET Framework C# compiler:

```powershell
cmake -S hailstone -B build-hailstone -G "Visual Studio 17 2022" -A x64 `
  -DHAILSTONE_SDK=C:/path/to/hailstone-client/sdk `
  -DHAILSTONE_IMGUI_DIR=C:/path/to/hailstone-client/build/_deps/imgui-src
cmake --build build-hailstone --config Release
```

`HAILSTONE_IMGUI_DIR` must be the Dear ImGui source the client builds with
(the client's `cmake/Dependencies.cmake` pins the version). Without it the
build fetches that version itself. The plugin's copy must match the client's
structure layout, because both operate on one `ImGuiContext` handed over by
`host->imgui_context`.

The outputs are `build-hailstone/Release/HSMono.dll` and
`build-hailstone/Smoke.dll`. The plugin uses only Hailstone's SDK for game
access. It loads the x64 Mono runtime at startup of a smoke session.

## Manual smoke test

Use a trusted x64 Mono distribution with its matching framework files. For
the MQ2Mono distribution, the x64 SGen library is under `mono-sgenlib_x64`,
and the framework's `lib` and `etc` directories come from the matching
x64 framework package. In the Hailstone data directory, arrange the files
as follows:

```text
plugins/HSMono/
  HSMono.dll
  managed/Smoke.dll
  runtime/
    mono-2.0-sgen.dll
    etc/
    lib/mono/4.5/mscorlib.dll
    ...
```

Enter a zone on a server that grants the plugin the `read` and `commands`
capabilities. Load it with `/plugin HSMono` and run
`/hmonosmoke start`. Look for successful managed `OnInit` and `OnPulse`
messages in Hailstone's log. Run `/hmonosmoke stop` and start again.
Exercise zoning and unloading the plugin while the smoke session is active,
then load the plugin again and start a fresh session. Each session should
report one successful pulse.

When upgrading from HailstoneMono, carry over its `runtime/`, `managed/`,
and `sentinel.json` to the HSMono directory and replace both plugin assemblies
with the new builds. Remove the old plugin from the active plugin directory
so only one host registers `/mono`. HSMono reads existing character UI
preferences from the previous plugin directory until it saves its own copy.

Keep supporting DLLs below the plugin directory's top level. Hailstone
scans the top level for native plugins. The manifest records the requested
capabilities. The manual layout is needed for this development build because
the current plugin package allows a single library.

The mock SDK/runtime test runs on Linux with
`hailstone/tests/run-mock.sh /path/to/hailstone-client/sdk`. It checks callback
ordering, snapshot marshalling, fault teardown and restart without a server.

Two Windows tests run the real Mono runtime without a game:

```powershell
# The bridge itself: two ImGui frames through a tiny managed caller.
build-hailstone/Release/ui_bridge_test.exe <runtime-dir> build-hailstone/UiSmoke.dll

# The production Sentinel host: the warrior trial, then a real Sentinel
# session with its XTarget/mini HUD/Songs/ToT windows drawn for three frames.
build-hailstone/Release/embedded_warrior_test.exe <config-dir>
```

`<config-dir>` is the prepared `plugins/HSMono` layout (a `runtime/`
and `managed/sentinel.dll`). The embedded test fails if the UI pass draws no
vertices, so a broken internal call or an unbalanced ImGui scope is caught
before the game. It checks that Sentinel reads the XTarget slot count from the bulk records
and that `/sentclear` can choose a hater with no selected target when the
bridge reads through the SDK's per-slot accessor.

## Sentinel warrior dry run

Build the managed adapter alongside the actual Sentinel sources with the
Developer PowerShell for Visual Studio:

```powershell
.\hailstone\Build-WarriorTrial.ps1 -SentinelRoot C:\path\to\plugins\Sentinel -OutputDirectory C:\path\to\output
.\hailstone\tests\Test-WarriorTrial.ps1 -Assembly C:\path\to\output\sentinel.dll
```

Place the resulting `sentinel.dll` in `plugins/HSMono/managed/` and
reload the updated native plugin. For the mini HUD logo, copy
`resources/sentinel_cat_128.png` from the Sentinel Lua folder into
`plugins/HSMono/managed/resources/`. Sentinel reads the PNG once per managed
session and uses the **S** button when the file is missing or cannot be decoded.

On a living warrior, select a living NPC
within 200 units, then run `/hmonosmoke warrior`. Check the log for proposed
targeting and combat actions. Select another NPC and run the same command
to capture a new target. Run `/hmonosmoke disengage` to see the proposed
release actions, and `/hmonosmoke stop` to join the decision task and close
the app domain.

The client captures game reads in the command callback. The worker consumes
only the captured strings, and the native pulse logs its result. The trial
uses Sentinel's existing warrior and command queue with default settings.
It excludes automatic pulling, smart taunt, UI, and broker startup. Queued
commands are traced and discarded. The log lists uncaptured reads.

To exercise the native bridge and the installed Mono framework without a game
session, build the optional `embedded_warrior_test` target and run it with the
prepared `plugins/HSMono` directory as its only argument. It supplies
a synthetic SDK snapshot and verifies engage, disengage, and a fresh managed
session after shutdown.

The dry run captures the player name and group roles through the SDK query
interface. For raid assist it uses the SDK roster. When multiple roster
members are flagged as main assist, the SDK does not expose their MQ slot
order, so this trial leaves raid assist slot 1 unresolved and logs that
limit. A zone transition closes the managed session and discards any
pending result. The next trial starts a fresh session.

## Opt-in live attack

`/hmonosmoke attack` submits one live decision for a solo warrior with
autoattack off and a living NPC selected within 200 units. It requires the
`actions` capability. If Sentinel proposes `/attack on`, the bridge checks
the current character, zone, target incarnation, and combat state before
delivering that command. Targeting and movement proposals are still traced
for inspection. A pending request expires after one second.

`/hmonosmoke disengage` and `/hmonosmoke stop` release autoattack started by
the bridge. Zoning, changing targets, and unloading also request release.
`/hmonosmoke warrior` continues to be the dry-run command.

## Running Sentinel C# in Hailstone

Use the same managed and native build steps above. In an active zone, unload
MQSentinel to free its command names, load HSMono, then run
`/mono load Sentinel`. The bridge starts Sentinel's regular C# dispatcher.
Use `/sentinel status`, `/engage local <id>`, or `/sentdisengage`.
`/mono unload Sentinel` releases Sentinel's commands and ends its managed session.
`/mono list` shows whether Sentinel is running, and `/mono version` shows the
plugin version. `run` and `start` alias `load`, `stop` aliases `unload`,
and `/mono Sentinel` is shorthand for loading Sentinel. `/mono unload all`
also unloads Sentinel. Sentinel is the only supported named application.
`/hmonosmoke` remains available for bridge diagnostics.

This host provides live Hailstone TLO queries, game command delivery,
incoming chat, and the per-character Hailstone plugin configuration
directory. Its native side also exports `OnUpdateImGui` and registers the
`MonoCore.E3ImGUI` internal-call surface Sentinel's windows use, so the
managed windows draw on Hailstone's ImGui context instead of MQ2Mono's.
The host stops the managed domain during zoning and starts it in the next
zone. Use one Sentinel implementation per client.

The bridge captures common player, target, and raid queries once per gameplay
pulse. The worker reads those values without waiting for a client-thread
handoff for each expression. Command guards and readiness checks use live
queries. The client still waits for the worker to yield, so an unusually
long turn can still delay the frame.

### UI status

The windows themselves (Options, Heartbeat, XTarget, Songs, ToT, AA Peers,
Stuck, Debug, mini HUD, encounter alert) are Sentinel's existing C# ImGui
code. HSMono supplies the drawing calls and keeps the open flags,
edit buffers, text filters and list clippers the MQ2Mono bridge kept per app
domain. Sentinel's own per-window guards and `Core.GuardUi` contain a bad
window or frame.

The peer roster is the ImGui **HeartbeatWindow** (`/sentpeers`). In
MacroQuest, MQSentinel's native RmlUi plugin owned that window and the C#
host skipped the UI command names; Hailstone has no such plugin, so this host
binds them (`/sentpeers`, `/sentstuck`, `/sentoptions`, `/sentmini`,
`/sentsongs`, `/senttt`, `/senttcp`, `/sentdebug`) and clears the
`NativeUiBridge` native-owner flags so the ImGui fallbacks draw. The
window has no fixed minimum width, and its right-click menu toggles
**Show Player Stats**, **Show Target**, **Show Buffs**, **Show Target Buffs**
and **Show Short Buff Window** independently. These choices, combat effects,
borderless mode, XTarget empty-slot visibility, and the Stuck panel's peer
filter save immediately to `sentinel-ui.json` in Hailstone's character plugin
configuration directory.

Sentinel's skill, discipline and activated AA commands use the host's ability
API. Its spell checks read the host's gem timer, including linked reuse, and
its aura checks read the server's active aura names. Bard song staging follows
the character's casting state. Command translation and host activation
failures are reported once per command in each Sentinel session.

The extended-target window reads Hailstone's `get_xtarget_data_bulk` for
slot role, ID, aggro and HP, and uses `get_spawn` for name, level, con and
distance. `/sentclear` uses those bulk slot roles and IDs to count and select
auto haters. When the bulk reply omits slots, the bridge reads those slots
through `get_xtarget`. The Hailstone snapshot gets its slot count from the bulk
reply. The MacroQuest snapshot uses `Me.XTargetSlots` to check its native read.

Chase resolves the leader to an in-zone spawn and uses Hailstone's
`/nav id` command. `/sentchase status` reports the last movement check,
including distance and any blocker.

Hailstone's `/stick` supports the `behind` position. Sentinel drops
`front`, `!front` and `pin` on Hailstone, and maps `behindonce` to
`behind`.

Known gaps, all display-only:

- **Spell icons** are blank. The icon cells live in the client's stock
  `A_SpellIcons` sheets; the plugin SDK exposes no texture handle, so the
  buff and song rows draw their text, borders and timers without an icon.
- **The mini HUD logo** uses its `S` text-button fallback; the plugin SDK
  has no file-texture loader.
- **Gradient text** falls back to solid colour text; that is MQ's
  `mq/imgui/AlphaMask.h`, which Hailstone does not have.
- **Window positions** are ImGui's per-context defaults. Hailstone's UI
  does not persist plugin window geometry across restarts.

The `ui` capability must be granted and is in the manifest's `caps`. The
managed assembly needs a `Sentinel.HailstoneHost.OnUpdateImGui` method; an
older assembly without it runs headless and says so in the log.


## Crew and casting

The C# host connects to the loopback Sentinel broker using
`port`, `token_file`, and `server` from `HSMono/sentinel.json`.
If that file supplies no `token_file`, it reads the existing `MQSentinel/sentinel.json`
beside the HSMono directory. Set the same server label as the rest of
the crew. `/sentinel status` reports connection status. `/engage <id>` and
`/sentclear` broadcast through the broker. The `local` forms stay on the
current client.

When nothing is listening on the port, the C# host starts the broker from
the `sentinel-broker.path` manifest and `broker/<hash>/sentinel-broker.exe`
deployed beside the managed `sentinel.dll`, with `--port <port>`. That is the
same content-addressed deployment MacroQuest Sentinel uses, and the broker
outlives the client, so the first client to zone in brings it up for the
whole crew. If the manifest or executable is missing, the attempt is
reported with `/sentinel status` and the connection retries as before; start
the broker by hand in that case. ACNet depends on the same listener and
does not start one itself.

Set `config_root` in `HSMono/sentinel.json` to share an existing
Sentinel configuration tree with MacroQuest. Point it at the
`config/Sentinel` directory above the server folders. Sentinel reads and
saves character settings in that tree, using the server label and the
existing spaced or unspaced server folder for each character. Shared mode
reads legacy Lua files in place and skips automatic migration.

Without `config_root`, on first use the host copies a matching `<Character>_cs.lua`
into Hailstone's character plugin configuration directory if it can find
the source beside the shared broker's MacroQuest installation or under
`cs_config_dir`. Hailstone keeps its own copy for later changes.

Sentinel's catalogue reads click spell IDs from the SDK for equipped items
and items carried in pack slots or bags. Entries use the item name as their
label and look up spell details by ID. Listing augment clickies requires
the SDK to resolve socket items.

The cleric can heal itself using `${Me.PctHPs}`. It reads raid candidates from
one Hailstone bulk snapshot per healing tick and uses the group roster when
outside a raid.

Sentinel's named spells resolve to memorized gems. Named clickies resolve
to possession slots, and the host maps skills and AA activations to native
ability calls. Item selection still requires the client's
`FindItem[…].TimerReady` support. The bridge adapts the MQ cast-readiness
queries to Hailstone's `Me.SpellReady` answer. Individual class rotations
may require additional TLOs or effect records.
