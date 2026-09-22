# ECharts Widget 1.0.0 — English Quick Reference

[中文完整教程](README.md)

ECharts Widget is an offline UMG chart plugin for **Unreal Engine 5.6 on Win64**. It bundles Apache ECharts 6.1.0 and echarts-gl 2.1.0 and renders through UE's `WebBrowserWidget`/CEF. The host page and JavaScript dependencies are local; no CDN is used.

`EChartsWidget` is independent from the legacy `ChartWidget`. Some Blueprint node names may look similar, but modules, widget classes, and data models differ. Both plugins can coexist. Version 1.0.0 does not promise drop-in compatibility with old `ChartWidget` Blueprints.

## Install and package

1. Close Unreal Editor.
2. Copy the complete folder to `<Project>/Plugins/EChartsWidget`. Do not overwrite `<Project>/Plugins/ChartWidget`.
3. Enable **ECharts Widget** under **Edit > Plugins > UI**. Its descriptor also enables **Web Browser Widget**. Restart when prompted.
4. Source projects must compile once for `Development Editor | Win64`. Blueprint-only projects without matching prebuilt binaries also need the UE 5.6 C++ toolchain.
5. Add `ECharts Widget` from the **ECharts** UMG palette and give it a non-zero size.

For an upgrade, close the editor, back up the old `EChartsWidget`, and replace the whole folder. Do not overlay two versions. If binaries are stale, remove only this plugin's `Binaries` and `Intermediate` and rebuild; do not delete project content, saves, or user data.

`Resources/Web/**`, `ThirdPartyLicenses/**`, the root `LICENSE`, and `THIRD_PARTY_NOTICES.md` are staged as NonUFS runtime dependencies. A Shipping package must retain them in the staged plugin tree.

## Five-minute quick start

In a Widget Blueprint:

1. Add an `ECharts Widget`, mark it as a variable, and use stretch anchors or explicit width/height.
2. On Construct, bind `On Chart Ready`, `On Chart Rendered`, `On ECharts Warning`, and `On ECharts Error` before initialization.
3. Call `Initialize ECharts(Template=SegmentedAreaLine, In Interaction Mode=ClickOnly)`.
4. On Ready, call:

   ```text
   Set Series Name(0, "Temperature")
   Set Legend Settings(Position=Auto, Orientation=Auto, FontSize=12)
   Add Data Point(0, 0.0, 21.5)
   Add Data Point(0, 1.0, 22.1)
   Apply ECharts Changes
   ```

Mutations update the UE-side cache. Submit with `Apply ECharts Changes`, or use `Set Auto Apply Enabled(true, Hz)`. Auto Apply is clamped to **1–30 Hz** (10 Hz default), coalesces dirty state, and waits for the prior browser ACK.

Legend entries are not entered manually: their count always matches non-empty data series, and `Set Series Name` changes each entry's label. Use `Set Legend Settings` to override visibility, position, orientation, spacing, font size, and icon size; `Reset Legend Settings` restores responsive defaults.

## Templates and fallback

| Template | Purpose | Without WebGL |
| --- | --- | --- |
| `SegmentedAreaLine` | 2D numeric/category line and area | Remains 2D |
| `Bar3DHeightMap` | XYZ bar height map | `Bar3DHeightMap2D` heatmap |
| `DataTableScatter3D` | XYZ scatter | `DataTableScatter2D` scatter |
| `CustomOption` | User ECharts option JSON | User-defined; arbitrary custom 3D options are not rewritten |

Read `Effective Template` or `On Chart Rendered(Requested Template, Effective Template)` to detect fallback. Fallback also sets `Last Warning` and emits `On ECharts Warning`. Built-in 3D templates force `autoRotate=false`.

## Core Blueprint API

Series indices are **0–3**. A series has one data type at a time. Numeric/category/3D values must be valid and finite; category X must not be blank. All series together are limited to 100000 points.

```text
Initialize ECharts(Template=SegmentedAreaLine, In Interaction Mode=ClickOnly) -> void

Add Data Point(Series Index, X, Y) -> bool
Set Series Data(Series Index, FEChartsDataPoint2D[]) -> bool
Append Series Data(Series Index, FEChartsDataPoint2D[]) -> bool
Get Series Data(Series Index) -> FEChartsDataPoint2D[]

Add Category Data Point(Series Index, X:String, Y) -> bool
Set Category Series Data(Series Index, FEChartsCategoryDataPoint[]) -> bool
Append Category Series Data(Series Index, FEChartsCategoryDataPoint[]) -> bool
Get Category Series Data(Series Index) -> FEChartsCategoryDataPoint[]

Set 2D Point Window(Enabled, Max Points=1000) -> void
Reset 2D Point Window() -> void

Set 3D Data(Series Index, FEChartsDataPoint3D[]) -> bool
Append 3D Data(Series Index, FEChartsDataPoint3D[]) -> bool
Get 3D Data(Series Index) -> FEChartsDataPoint3D[]

Clear Series(Series Index) -> bool
Clear All() -> void
Set Series Name(Series Index, Name) -> bool
Set X Axis Mode(FollowLatestWindow | ShowAll | Category) -> void
Apply ECharts Changes() -> void
Set Auto Apply Enabled(Enabled, Max Updates Per Second=10.0) -> void
```

`FEChartsDataPoint3D` contains `X, Y, Z, ColorValue, SymbolSizeValue`. Native Bar3D/Scatter3D derives the visualMap range from `ColorValue`; Scatter3D uses `SymbolSizeValue`. A native-only 3D option has no 2D `xAxis`, `yAxis`, or `grid`. The Bar3D heatmap fallback colors by Z, while Scatter2D colors by `ColorValue`. Data and Auto Apply merge series/legend/visualMap without rebuilding `grid3D`, preserving the user's camera. Ordinary category Apply creates a union label domain; duplicate X values in one series use the last value. Category streaming preserves repeated labels in append order.

`Set 2D Point Window` is disabled by default and clamps Max Points to 1–100000. Each non-streaming Numeric2D/Category series (0–3) owns an independent fixed-capacity true ring: enabling or shrinking keeps its newest N points immediately; Add/Append overwrite the oldest point in stable logical order; Set and sorted DataTable snapshots keep their suffix. `Reset 2D Point Window` restores Disabled/1000. Disable/Reset preserves the current points and permits later growth, but discarded points are never recovered. No mouse drag, wheel handler, or `dataZoom` is installed; the payload contains only the retained cache and value/category axes automatically rescale to it.

Data3D is completely unaffected: the node never trims, converts, or changes X/Y/Z/ColorValue/SymbolSizeValue, and the existing global 100000-point limit remains strict. During Preparing/Playing/Paused, DataTable Time Series Series 0 is controlled **exclusively** by the existing Time Series Window; the two limits are not combined with `min`. Ordinary 2D Series 1–3 remain under the 2D point window. After Stop/Completed, Series 0 returns to the ordinary 2D policy and submits a final full payload.

Blueprint example: `Set 2D Point Window(true, 100)` → `Set Auto Apply Enabled(true, 10)` → repeatedly call `Add Data Point(0, Elapsed, FPS)`.

### Legend settings

`Set Legend Settings(Settings)` and `Reset Legend Settings()` are under `ECharts|Legend`. `FEChartsLegendSettings` defaults to visible, Auto position/orientation, font size 12, gap 10, icon 25×14, and Custom X/Y 50%/5%. C++ clamps these values again. Auto uses a centered horizontal top legend at widths ≥720 CSS px and a vertical right legend below that threshold. Top/Bottom/Left/Right/Custom and Horizontal/Vertical provide explicit overrides. Settings are transactional: Blueprint Read Only state changes only after a successful `On Legend Settings Applied` ACK; failure preserves last-good settings. The latest requested candidate is cached during Loading and replayed after Release/Rebuild without Tick or polling. Blueprint legend settings remain the final override even when CustomOption supplies its own legend object or array.

There is no history database, playback cache, or `Set History Capacity` API. Both point-window APIs retain only their current in-memory window; keep long-term history in the game's own data layer.

### Interaction

`Set Interaction Mode(Mode)` applies when Ready or is cached until Ready.

| Mode | Behavior |
| --- | --- |
| `Disabled` | tooltip hidden/none, legend selection disabled, series silent, 3D rotate/zoom/pan 0 |
| `ClickOnly` (default) | tooltip on click, legend/series active, 3D rotate 1, zoom/pan 0 |
| `FullHover` | tooltip on mousemove or click, legend/series active, 3D rotate/zoom/pan 1 |

All modes force 3D `autoRotate=false`. Rules recurse into root, `baseOption`, timeline `options`, and media options. They overwrite tooltip/legend/series/viewControl fields; switching modes does not restore earlier CustomOption values. There is no UE mouse polling or permanent interaction Tick.

### Events and runtime

Runtime states are `Uninitialized`, `Loading`, `Ready`, and `Error`. Each Initialize creates a generation; stale messages are ignored. Ready and Rendered fire once per generation.

Important events:

- `On ECharts Applied(Revision, Point Count)`
- `On Interaction Mode Applied(Mode, Success, Message)`
- `On Legend Settings Applied(Success, Message)`
- `On Option Applied(Success, Message)`
- `On JavaScript Result(Request Id, Success, Message)`
- `On Data Table Load Progress`, `On Data Table Loaded`, `On Data Table Load Cancelled`
- `On Data Table Streaming Started/Completed/Stopped`
- `On Data Table Stream Progress(Current, Total, Loop)`, `On Data Table Stream Looped(Loop)`

`On ECharts Error` also reports recoverable data validation failures, so inspect `Runtime State`. A host `ERROR` makes the current generation terminal. Initialize again, or let Release/Rebuild create a new generation.

## DataTable snapshot

Supported top-level scalar columns:

- Numeric: integer and floating properties except enum properties and enum-backed bytes; usable as numeric X/Y/Z/Color/SymbolSize.
- Category: `FString`, `FName`, `FText`, enums; usable as 2D X.
- Unsupported: bool, `FDateTime`, nested structs, objects, arrays/collections, non-scalar fixed arrays.

Date text in String/Name/Text is a category label; it is not parsed. Use numeric seconds for a numeric time axis.

`FEChartsDataTableMapping` fields are `X, Y, Z, Color, SymbolSize, Order`. 2D requires supported X and numeric Y. 3D requires numeric X/Y/Z; Color and SymbolSize are optional numeric fields. Defaults are `ColorValue=Z` and `SymbolSizeValue=12`. `XAscending` is stable numeric ascending or case-sensitive category lexical order; `RowName` follows FName plain-name/number ordering.

```text
Get ECharts DataTable Columns(Table)
  -> Set DataTable Mapping(Table, Mapping)
  -> Load Data Table(Rows Per Frame=256)
  -> progress
  -> On Data Table Loaded(Succeeded, Skipped)
```

`Rows Per Frame` is clamped to 1–4096. Invalid rows (blank category, missing data, NaN/Infinity, conversion failure) are skipped and counted. Snapshot load replaces Series 0, keeps Series 1–3, and automatically Applies even when Auto Apply is off. Completion waits for the exact browser ACK.

## DataTable Time Series

```text
Set DataTable Mapping(Table, Mapping)
Set Time Series Enabled(true)
Set Time Series Window(1000)
Start Data Table Streaming(0.1, 1, false, 256)
```

Limits/defaults:

- Window: 1–100000, default 1000.
- Interval: 0.01–60 seconds, default 0.1.
- Rows Per Step: 1–256, default 1.
- Preparation Rows Per Frame: 1–4096, default 256.
- Source: at most 100000 rows and an estimated 16 MiB JSON representation.

States are `Stopped`, `Preparing`, `Playing`, `Paused`, `Completed`, `Error`. Sort keys are read incrementally on the Game Thread and a worker only performs the stable row-name sort. The Game Thread then uses `Preparation Rows Per Frame` to reflect and convert rows and builds each small delta with no more than `Rows Per Step` points. The first batch may display before all rows are prepared, without blocking the Game Thread.

If preparation finishes with zero valid rows, the stream enters `Error` with `DataTable contains no valid rows to stream.`

Steady-state updates send small deltas and use a true ring. At most two unacknowledged deltas are queued; production pauses under browser backpressure. `Loop=true` does not clear the chart: the cursor returns to row zero, new rows append, and old rows fall out of the ring. Stop releases timers/prepared source but preserves the visible window. Release/Rebuild suspends and resumes preparation/playback and replays the presentation state.

## Option JSON

`Set ECharts Option JSON(Option Json) -> bool` accepts a valid top-level JSON object up to 16 MiB. JSON cannot contain functions, comments, `undefined`, NaN/Infinity, or an `option =` prefix. `true` means local validation passed; wait for `On Option Applied` for ECharts semantic success.

Applying an option is transactional. Success commits `CustomOption` as last-good. A normal `setOption` failure rebuilds the chart and restores the prior option while remaining Ready. If rollback/rebuild itself fails, the host emits a terminal Error. Last-good CustomOption is replayed after Release/Rebuild.

```json
{
  "animation": false,
  "backgroundColor": "transparent",
  "xAxis": { "type": "category", "data": ["A", "B", "C"] },
  "yAxis": { "type": "value" },
  "series": [{ "name": "Value", "type": "line", "data": [3, 1, 4] }]
}
```

## Raw JavaScript security

`Execute ECharts JavaScript(JavaScript, Out Request Id) -> bool` is accepted only while Ready and is limited to 1 MiB. The source receives `chart`, `echarts`, and `host`; it is not cached or replayed.

```javascript
if (!chart || !echarts || !host) throw new Error('missing trusted API');
chart.setOption({ title: { text: 'Trusted local diagnostic' } });
host.resize();
```

> [!CAUTION]
> Raw JavaScript is privileged and **not sandboxed**. It runs through `new Function`; the arguments do not prevent access to `window`, `globalThis`, or `document`, and code can mutate the page or navigate. `connect-src 'none'` restricts fetch/XHR/WebSocket-like connections but does not prohibit navigation and is not a general security boundary.
>
> Execute only developer-reviewed constant scripts. Never pass or concatenate user input, network data, chat, DataTable cells, configuration, save-game content, or other untrusted text. The plugin exposes no UE UObject bridge; do not add one. Prefer typed data APIs and pure Option JSON.

## Performance and limits

- No Sleep or synchronous CEF wait is used.
- Snapshot reflection reads are Game-Thread frame-budgeted; its worker performs stable sorting, conversion, and complete-payload construction.
- A stream worker only performs the stable row-name sort. The Game Thread frame-budgets reflection/conversion and builds deltas of at most `Rows Per Step`; a true ring and at most two pending deltas bound steady-state work.
- Auto Apply allows one in-flight revision and coalesces newer dirty state.
- Stream uses deltas, at most two pending batches, and a true ring.
- 4 series, 100000 total points, 16 MiB data/option JSON, 1 MiB Raw JS.

Start with 1–10 Hz and a 200–2000 point window for dashboards. Prefer 1–5 Hz and smaller windows for 3D. Use 30 Hz only for small datasets after profiling the actual Shipping build. Lower DataTable rows per frame if the Game Thread hitches.

## Troubleshooting

| Symptom | Checks |
| --- | --- |
| Blank widget | Non-zero UMG size; Initialize called; Ready/Rendered received; inspect Runtime State/Last Error. |
| Browser unavailable | Enable Web Browser Widget, restart, verify complete `Resources/Web`; UE 5.6 Win64 only. |
| 3D becomes 2D | Expected WebGL fallback; inspect Warning and Effective Template. |
| Option rejected | Remove `option =`, functions/comments/trailing comma; require top-level object under 16 MiB; inspect On Option Applied. |
| DataTable mapping fails | Discover columns; numeric Y/Z/Color/Size; numeric X for 3D; bool/FDateTime unsupported. |
| Stream does not move | Enable Time Series, validate mapping/type, inspect Preparing/Paused/Error, reduce load if backpressured. |
| Shipping host missing | Verify staged NonUFS `Resources/Web`, filter config, and Build.cs. |
| Raw JS fails | Call only when Ready; correlate Request ID with On JavaScript Result; do not weaken CSP. |

Search logs for `__UE_ECHARTS_`: READY, RENDERED, WARNING, ERROR, APPLIED, OPTION_RESULT, INTERACTION_RESULT, and JAVASCRIPT_RESULT include generation/revision/request details.

## Build and tests

From the plugin root:

```powershell
node --test Tests/Web/assets.test.js Tests/Web/templates.test.js Tests/Web/chart-host.test.js
```

UE logic tests can run with NullRHI:

```powershell
$Project = "D:\Work\YourProject\YourProject.uproject"
& "$env:UE_ROOT\Engine\Binaries\Win64\UnrealEditor-Cmd.exe" `
  $Project -unattended -nop4 -NoSplash -NoSound -NullRHI `
  '-ExecCmds=Automation RunTests EChartsWidget;Quit' `
  '-TestExit=Automation Test Queue Empty'
```

Real CEF integration tests require a rendered D3D12 Slate browser and must not use NullRHI:

```powershell
$Project = "D:\Work\YourProject\YourProject.uproject"
& "$env:UE_ROOT\Engine\Binaries\Win64\UnrealEditor-Cmd.exe" `
  $Project -unattended -nop4 -NoSplash -NoSound -RenderOffscreen -d3d12 `
  '-ExecCmds=Automation RunTests EChartsWidget;Quit' `
  '-TestExit=Automation Test Queue Empty'
```

Build a distributable plugin with:

```powershell
$Plugin = "D:\Work\YourProject\Plugins\EChartsWidget\EChartsWidget.uplugin"
$Package = "D:\Build\EChartsWidget-1.0.0"
& "$env:UE_ROOT\Engine\Build\BatchFiles\RunUAT.bat" BuildPlugin `
  "-Plugin=$Plugin" "-Package=$Package" -TargetPlatforms=Win64
```

`Z:\Project\UE5_Plugin\Chart\ChartPlugin\ChartPlugin.uproject` is only this repository's development example, not a required user path.

## License

- Plugin: [MIT](LICENSE)
- Third-party details: [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)
- Pinned sources and SHA-256: [vendor manifest](Resources/Web/vendor/manifest.json)

Keep `LICENSE`, `THIRD_PARTY_NOTICES.md`, and `ThirdPartyLicenses` when redistributing the plugin.

The root `LICENSE` is explicitly staged as a NonUFS runtime dependency alongside the notices and third-party license directory.
