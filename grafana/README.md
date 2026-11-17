# Weather event dashboard

Import [Weather2-events.json](./Weather2-events.json) for the redesigned view.
The [original export](./Weather2-1794746009671.json) is unchanged. The new
dashboard uses a different title and UID, so it can sit alongside the original.

## Layout

- **At a glance:** the original five summary cards (temperature, wind speed,
  gust, hourly rain rate, River Chew), retaining their colour thresholds.
  Each now has a sparkline covering the selected time range, rather than an
  instant query that cannot draw a trend.
- **Event timeline:** temperature, pressure, humidity, wind speed/gust,
  wind direction, rain intensity, rolling 24-hour rainfall, then river level.
  Each chart occupies the full width, with the same 80-pixel left-axis width
  and bottom legend, keeping the time axes aligned. No chart overrides the
  dashboard time range.
- **Statistics:** latest, minimum, maximum and mean appear below each line
  chart for the selected range. Wind direction shows only its latest value:
  an arithmetic average of compass bearings is misleading.
- **Context:** compact one-, six- and 24-hour rain totals and one-hour river
  change remain at the bottom.

Only built-in Grafana panels and the Prometheus datasource are used. The old
heatmaps, duplicate gauges, wind-rose plugin and external River library panel
are replaced by time-series charts. The design targets the export's Grafana
9.5 / schema 38 baseline.

## Import

1. In Grafana, choose **Dashboards > Import**, then upload
   [Weather2-events.json](./Weather2-events.json).
2. Select both Prometheus inputs:

   | Import input | Original datasource UID | Used for |
   | --- | --- | --- |
   | Weather / summary Prometheus | `w18_-Nanz` | Summary cards and temperature history |
   | History Prometheus | `d74125a9-6863-4b39-91cd-b477433eb79d` | Other timeline charts and bottom statistics |

   These preserve the query-level source bindings in the original export,
   even where its panel-level datasource disagreed. Choose the same datasource
   for both inputs if they are actually the same server. If they differ, their
   metrics need to describe the same station for meaningful comparison.
3. Keep the UID `weather4` (title `Weather4`) so it matches the deployed
   dashboard and does not replace the old `weather2` one. Re-importing over
   an existing `weather4` dashboard prompts to overwrite it.
4. Start with **Last 24 hours**. Hover over a chart for the shared vertical
   crosshair; drag-select an event to zoom the dashboard's charts together.
   The timezone is the viewer's browser timezone.

Use the import screen, not direct provisioning: its datasource prompts replace
the `${DS_WEATHER}` and `${DS_HISTORY}` placeholders. Provisioned deployments
must substitute those references themselves.

## Reading the values

- Summary numbers use the **last non-null returned sample** in the selected
  range. With a historical range they describe that period, not the live
  weather now. A trailing data gap can leave an older value displayed; inspect
  chart timestamps when freshness matters. Missing series are not filled
  with zero.
- Legend statistics are calculated from the samples returned for the plotted
  range. Prometheus query step/resolution can miss brief extremes, particularly
  over long ranges: these are chart statistics, not guaranteed exact extrema
  of every stored scrape. Temperature is no longer smoothed over five minutes.
- The rain summary keeps `increase(rain_day[60m])`: rainfall in the preceding
  hour, numerically equal to its average rate in mm/hr. The rain chart also
  adds `rain_min_rate * 60` for more responsive last-minute intensity, converting
  the documented mm/min metric to mm/hr. If an older source does not expose
  `rain_min_rate`, that series will be absent; the existing hourly line remains.
- Rain totals preserve the original `increase(rain_day[...])` queries.
  They are rolling-window estimates, not totals since the selected range
  began. `rain_day` resets daily; Prometheus treats decreases as counter
  resets and extrapolates, so gaps or unexpected resets can distort totals.
  This redesign does not change the station's metrics or rain calculations.
- Bottom statistics are instant queries evaluated at the dashboard range end.
  Their one-, six- and 24-hour lookbacks can extend before its start. All four
  use a fixed `mm` suffix with one decimal place, so Grafana never rescales
  small values to micrometres. River change is `delta(riverlevel[1h]) * 1000`,
  converting metres to millimetres.
- Direction is plotted as points without averaging or connecting across
  north (359 to 1 degrees). It may retain the last direction during calm wind.
- Length units are fixed suffixes rather than Grafana's rescaling length
  units: rainfall is always shown in `mm` and river level always in `m`, so a
  level of 0.45 m is never displayed as 450 mm. River level uses `suffix:m`
  because a bare `m` is Grafana's built-in ID for minutes.
- River summary colours are retained for familiarity, not presented as
  official flood-warning thresholds. Pressure and river charts auto-scale
  vertically so smaller changes are visible.

The JSON can be checked offline for layout and query configuration, but live
rendering and metric availability must be checked after import into Grafana.
