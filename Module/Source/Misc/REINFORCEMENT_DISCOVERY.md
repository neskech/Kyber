# Reinforcement spawn-card mapping

Always-on path (no env vars): cost-row `Character`/`Hero` binds the kit on the entity bus; sibling `TextElementData.Text` gets `cur/start`; same-bus `WSUIWidget` / list-item gets `Enabled`/`IsEnabled` plus Enable/Disable at `cur == 0`. Overlay `GC_COUNT_OVERLAY` is the fallback list.

Ghidra `search_strings` on `starwarsbattlefrontii.exe` does not finish (232 MB packed `.tlsa`; auto-analysis never reports complete). Use the PE byte scan and runtime `CollectFields`. Evidence ids are in `.cursor/memory/designs/kyber.md` (Spawn-menu mapping).

## Optional discovery

Set before launching Battlefront II (all default off):

- `GC_TEXT_DISCOVERY=1` — entity and localization logging (`[GCTextDiscovery]`)
- `GC_WIDGET_DISCOVERY=1` — UI screen message dump (`[WidgetDiscovery]`)
- `GC_COUNT_OVERLAY=0` — hides the fixed debug text list
- `GC_TEXT_FIELD` / `GC_TEXT_SOURCE_FIELD` / `GC_TEXT_BIND` / `GC_TEXT_MODE` — overrides for the always-on writer

`GC_TEXT_FIELD` defaults to `Text`. `GC_TEXT_SOURCE_FIELD` defaults to that same field.
