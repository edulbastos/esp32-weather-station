// Overview (FR-4.1): one card per station, one card per sensor inside.
"use strict";

(() => {
  const { $, el } = WS;
  let stations = [];
  let reloadTimer = null;

  async function load() {
    try {
      const d = await WS.api("/api/stations");
      WS.setServerTime(d.server_time);
      WS.showBanner(d.mqtt_connected);
      $("user").textContent = d.user;
      stations = d.stations;
      $("status").textContent = "Updated " + new Date().toLocaleTimeString([], { hour12: false });
      $("status").classList.remove("error");
      render();
    } catch (e) {
      $("status").textContent = "Frontend unreachable, retrying…";
      $("status").classList.add("error");
      clearTimeout(reloadTimer);
      reloadTimer = setTimeout(load, 10000);
    }
  }

  // info and metar messages are rare: reload once, after a burst.
  function reloadSoon() {
    clearTimeout(reloadTimer);
    reloadTimer = setTimeout(load, 1000);
  }

  function render() {
    const list = $("stations");
    list.replaceChildren(...stations.map(stationCard));
    $("empty").classList.toggle("hidden", stations.length > 0);
  }

  function rerender(st) {
    const old = document.querySelector(`[data-device="${st.device_id}"]`);
    if (old) old.replaceWith(stationCard(st));
  }

  function stationCard(st) {
    const state = WS.stationState(st);
    const card = el("article", "card station" + (state === "online" ? "" : " greyed"));
    card.dataset.device = st.device_id;

    const head = el("div", "station-head");
    const link = el("a", "station-name", st.name || st.device_id);
    link.href = "/station/" + encodeURIComponent(st.device_id);
    head.append(link, WS.stateBadge(state));

    const meta = [];
    if (st.name) meta.push(st.device_id);
    const newest = newestReading(st);
    if (state === "offline") {
      meta.push("last seen " + WS.fmtAge(WS.now() - st.last_seen));
    } else {
      if (newest && WS.num(newest.rssi_dbm)) meta.push(WS.signal(newest.rssi_dbm));
      meta.push(st.last_reading != null ? "updated " + WS.fmtAge(WS.now() - st.last_reading) : "no reading yet");
    }
    card.append(head, el("div", "station-meta", meta.join(" · ")));
    for (const g of st.sensors) card.append(sensorCard(g));
    return card;
  }

  function newestReading(st) {
    let best = null;
    for (const g of st.sensors) {
      if (g.latest && (!best || g.latest.received_at > best.received_at)) best = g.latest;
    }
    return best;
  }

  function mini(value, unit, sub, arrow) {
    const box = el("div", "mini");
    const line = el("div");
    line.append(el("span", "mini-value", value), el("span", "unit", unit));
    if (arrow) line.append(el("span", "mini-arrow", arrow));
    box.append(line, el("div", "sub", sub || ""));
    return box;
  }

  function sensorCard(g) {
    const r = g.latest;
    const box = el("div", "sensor");
    const title = el("div", "label", WS.sensorTitle(g));
    if (WS.calibration(g)) {
      const tag = el("span", null, " · calibrated");
      tag.title = WS.calibration(g);
      title.append(tag);
    }
    const values = el("div", "mini-values");
    let hasPressure = false;
    for (const ch of WS.channelsOf(g)) {
      if (ch.id === "temperature") {
        values.append(mini(WS.fmt(r && r.temperature_c), "°C", "temperature"));
      } else if (ch.id === "humidity") {
        values.append(mini(WS.fmt(r && r.humidity_rh), "%RH",
          r && WS.num(r.dew_point_c) ? "dew point " + WS.fmt(r.dew_point_c) + " °C" : "humidity"));
      } else if (ch.id === "pressure") {
        hasPressure = true;
        const p = r && (WS.num(r.pressure_msl_hpa) ? r.pressure_msl_hpa : r.pressure_hpa);
        const arrow = WS.trendArrow(r);
        values.append(mini(WS.fmt(p), "hPa",
          WS.trendText(r).replace(/^\S+ /, "") || (r && WS.num(r.pressure_msl_hpa) ? "sea level" : "pressure"), arrow));
      } else {
        values.append(mini(WS.fmt(WS.unknownValue(r, ch)), ch.unit || "", ch.id));
      }
    }
    box.append(title, values);
    if (hasPressure && r && r.forecast) box.append(el("div", "forecast-line", r.forecast));
    if (!r) box.append(el("div", "sub", "no reading yet"));
    return box;
  }

  function onEvent(type, ev) {
    const st = stations.find((s) => s.device_id === ev.device_id);
    if (!st) return reloadSoon();  // a new station (FR-3.3)
    if (type === "reading") {
      const g = st.sensors.find((x) => x.sensor_ids.includes(ev.sensor_id));
      if (!g) return reloadSoon();  // a new sensor
      if (g.sensor_id !== ev.sensor_id && g.latest && g.latest.time > ev.reading.time) return;
      g.latest = ev.reading;
      st.last_reading = ev.reading.received_at;
      st.last_seen = ev.reading.received_at;
      rerender(st);
    } else if (type === "status") {
      st.status = ev.status;
      st.status_time = ev.time;
      st.last_seen = Math.max(st.last_seen, ev.time || 0);
      rerender(st);
    } else {
      reloadSoon();
    }
  }

  WS.live({
    event: onEvent,
    reload: load,
    mqtt: WS.showBanner,
    down: () => {
      $("status").textContent = "Live updates interrupted, reconnecting…";
      $("status").classList.add("error");
    },
  });
  WS.keepFresh(load);
  // Ages and states change with time alone (FR-4.4).
  setInterval(() => stations.forEach(rerender), 5000);
  load();
})();
