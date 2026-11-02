# Reinforcement label discovery

Goal: find out how to put `cur/start` into the spawn button's point-cost label.

## 1. Collect data (no writes)

Set before launching Battlefront II (all optional, default off):

- `GC_TEXT_DISCOVERY=1` — entity and localization logging (`[GCTextDiscovery]`)
- `GC_WIDGET_DISCOVERY=1` — UI screen message dump (`[WidgetDiscovery]`)
- `GC_COUNT_OVERLAY=0` — optional, hides the fixed debug text list

Steps:

1. Join a Galactic Conquest match as usual.
2. Open the spawn screen while you can afford the buttons. Wait about 10 s.
3. Close it, get below the cost of a kit (or use a kit that is too expensive), open the spawn screen again. Wait about 10 s.
4. Copy every `[GCTextDiscovery]` and `[WidgetDiscovery]` line from the Kyber log.

Lines to expect:

- `created entity=... data=<Type> isComponent=... fields=[ Name:Type ... ]` — field list of each wrapped data type
- `changed entity=... property=<Name> valueType=... asChars/asPtr/hash...` — text and id changes (TextElementData, BattlepointUnitCostEntityData)
- `loc id=<id> -> <text>` — localization lookups during the 10 s after a UI screen opened

Output is limited to 200 lines per second and 300 `loc` lines per screen open.

## 2. Try the writer on one button

Read the TextElementData field list first. Then set:

- `GC_TEXT_FIELD=<field>` — the TextElementData field to override
- `GC_TEXT_MODE=string` (default) or `localizedId` — must match the field type (`CString` or `LocalizedStringId`)
- `GC_TEXT_SOURCE_FIELD=<CString field>` — optional; field whose text is read for binding and as the cost text (defaults to `GC_TEXT_FIELD`; it must be a CString field)
- `GC_TEXT_BIND=<substring>=<kitShortName>[,...]` — binds a text element whose current text contains the substring to that kit (short name = text after the last `/` of the kit in the `GCR1` line)

Example: the cost label of the button shows `30` and the kit short name is `Heavy`: `GC_TEXT_BIND=30=Heavy`.

Look for `[GCTextDiscovery] bound ...` and `[GCTextDiscovery] wrote ...`. If the button shows `<cost>  <cur>/<start>`, the write works. If nothing changes or the game crashes, send the log lines.

In `localizedId` mode the id is `GC_REINF_<kit>_<cur>_<start>` and the localization hook returns the formatted text for it.
