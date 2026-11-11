# Track End Alert

An AZ-MODS mod for the XDJ-AZ. When a playing track has **10, 15, 30, 45 or 60 seconds** left
(your choice), a red banner appears on the screen for **2 seconds**, on every screen.

Settings: **Mod Menu > Community Mods > Track End Alert**: on/off, the delay (default 30 s)
and a "Tester l'alerte" button. The alert fires once per track and deck, and re-arms when the
deck is emptied or the playhead goes back above the delay.

Core services only (no hooks, controls, audio or network), so it loads on every firmware.

## Build

```sh
<sdk>/tools/azmod-pack build .
<sdk>/tools/azmod-pack package . --github maximevincentclaudeai/track-end-alert
make test AZ_SDK=<sdk>
```
