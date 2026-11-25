// Shared code of the overview and the station page. MQTT values are only ever
// inserted as text (NFR-3.2); SVG markup is built from numbers only.
"use strict";

const WS = (() => {
  const $ = (id) => document.getElementById(id);

  function el(tag, cls, text) {
    const e = document.createElement(tag);
    if (cls) e.className = cls;
    if (text != null) e.textContent = text;
    return e;
  }

  // Server clock: ages and states are computed against it, not the browser clock.
  let offset = 0;
  const now = () => Date.now() / 1000 + offset;
  function setServerTime(t) { if (t) offset = t - Date.now() / 1000; }

  const num = (x) => typeof x === "number" && isFinite(x);
  const fmt = (x, digits = 1) => (num(x) ? x.toFixed(digits) : "–");
  const signed = (x, digits = 1) => (num(x) ? (x >= 0 ? "+" : "") + x.toFixed(digits) : "–");

  function fmtAge(s) {
    if (s == null) return "never";
    s = Math.max(0, Math.round(s));
    if (s < 60) return s + " s ago";
    if (s < 3600) return Math.round(s / 60) + " min ago";
    if (s < 172800) return +(s / 3600).toFixed(s < 36000 ? 1 : 0) + " h ago";
    return Math.round(s / 86400) + " d ago";
  }

  function fmtTime(unix) {
    return new Date(unix * 1000).toLocaleString([], { dateStyle: "short", timeStyle: "medium", hour12: false });
  }

  function fmtDuration(s) {
    return s >= 3600 ? +(s / 3600).toFixed(1) + " h" : s >= 60 ? Math.round(s / 60) + " min" : s + " s";
  }

  // Station FR-11.2: ICAO standard atmosphere.
  const mslFactor = (alt) => Math.pow(1 - 2.25577e-5 * alt, -5.25588);

  // Station FR-5.7: bars from the RSSI (-60 / -70 dBm, main/config.h).
  function signal(rssi) {
    if (!num(rssi)) return "";
    const level = rssi >= -60 ? 3 : rssi >= -70 ? 2 : 1;
    return "▂▄▆".slice(0, level) + " " + rssi + " dBm";
  }

  // Station FR-13.1: "↓ -1.8 hPa in 3 h, falling".
  const ARROWS = { rising: "↑", falling: "↓", steady: "→" };
  function trendText(r) {
    if (!r || r.pressure_trend == null || !num(r.pressure_trend_hpa)) return "";
    const dir = r.pressure_trend.split("_")[0];
    return (ARROWS[dir] || "") + " " + signed(r.pressure_trend_hpa) + " hPa in 3 h, " +
      r.pressure_trend.replace(/_/g, " ");
  }
  function trendArrow(r) {
    return r && r.pressure_trend ? ARROWS[r.pressure_trend.split("_")[0]] || "" : "";
  }

  // FR-4.4: online, no data, offline.
  const STATE_LABEL = { online: "online", no_data: "no data", offline: "offline" };
  function stationState(st) {
    if (st.status === "offline") return "offline";
    const limit = 3 * (st.publish_period_s || 60) + 10;
    const recent = st.last_reading != null && now() - st.last_reading <= limit;
    if (recent) return "online";
    return st.status === "online" ? "no_data" : "offline";
  }
  function stateBadge(state) {
    const s = el("span", "state state-" + state);
    s.append(el("span", "dot"), el("span", null, STATE_LABEL[state]));
    return s;
  }

  // Sensor title: "bme280-3fa91c07 · position 0 · i2c@0x76".
  function sensorTitle(g) {
    return [g.sensor_id, g.index != null ? "position " + g.index : null, g.address].filter(Boolean).join(" · ");
  }

  // Channels of a sensor from the info message; the BME280 ones when it has not arrived yet.
  const DEFAULT_CHANNELS = [
    { id: "temperature", unit: "°C" }, { id: "humidity", unit: "%RH" }, { id: "pressure", unit: "hPa" },
  ];
  const KNOWN = { temperature: "temperature_c", humidity: "humidity_rh", pressure: "pressure_hpa" };
  const channelsOf = (g) => (g.channels && g.channels.length ? g.channels : DEFAULT_CHANNELS);
  // Value of a channel the frontend does not know: the payload field "<id>_<unit>".
  function unknownValue(reading, ch) {
    if (!reading) return null;
    const key = Object.keys(reading).find((k) => k.startsWith(ch.id + "_") && num(reading[k]));
    return key ? reading[key] : null;
  }

  // Calibration offsets of a sensor (station FR-11.1), as a tooltip; null when none.
  function calibration(g) {
    const parts = (g.channels || []).filter((c) => num(c.offset) && c.offset !== 0)
      .map((c) => c.id + " " + signed(c.offset, c.id === "pressure" ? 1 : 2) + " " + c.unit);
    return parts.length ? "Offsets: " + parts.join(", ") : null;
  }

  async function api(path) {
    const r = await fetch(path, { cache: "no-store", credentials: "same-origin" });
    if (r.status === 401) {
      location.href = "/login?next=" + encodeURIComponent(location.pathname + location.search);
      throw new Error("not logged in");
    }
    if (!r.ok) {
      const e = new Error(r.status);
      e.status = r.status;
      throw e;
    }
    return r.json();
  }

  // FR-4.3: one SSE connection; on a drop reconnect after 5 s, doubling up to
  // 60 s, and reload everything, since events may have been missed.
  function live(handlers) {
    let es = null, delay = 5000, reconnecting = false, timer = null;
    function open() {
      es = new EventSource("/api/events");
      es.addEventListener("hello", (e) => {
        delay = 5000;
        const d = JSON.parse(e.data);
        setServerTime(d.server_time);
        handlers.mqtt(d.mqtt_connected);
        if (reconnecting) {
          reconnecting = false;
          handlers.reload();
        }
      });
      for (const type of ["reading", "status", "info", "metar"]) {
        es.addEventListener(type, (e) => handlers.event(type, JSON.parse(e.data)));
      }
      es.addEventListener("mqtt", (e) => handlers.mqtt(JSON.parse(e.data).connected));
      es.onerror = () => {
        es.close();
        reconnecting = true;
        handlers.down();
        clearTimeout(timer);
        timer = setTimeout(async () => {
          try { await api("/api/me"); } catch (err) { /* a 401 goes to the login page */ }
          open();
        }, delay);
        delay = Math.min(delay * 2, 60000);
      };
    }
    open();
  }

  // FR-4.3: reload when the page becomes visible again and every 30 minutes.
  function keepFresh(reload) {
    document.addEventListener("visibilitychange", () => {
      if (document.visibilityState === "visible") reload();
    });
    setInterval(reload, 30 * 60 * 1000);
  }

  function showBanner(connected) {
    $("banner").classList.toggle("hidden", connected !== false);
  }

  // FR-4.2 (since v1.0.3): the value axis of a chart. Grid lines at round values, a step of
  // 1, 2 or 5 times a power of ten and at least 10^-digits, with the data
  // range widened to whole steps, so each line lies exactly at the value of
  // its label: the smallest such step that needs at most 6 intervals, and at
  // least 2 intervals.
  function niceAxis(min, max, digits) {
    const minStep = Math.pow(10, -digits);
    if (max - min < minStep) { min -= minStep / 2; max += minStep / 2; }
    let step, k0, k1;
    for (let i = 0; ; i++) {
      step = [1, 2, 5][i % 3] * Math.pow(10, Math.floor(i / 3)) * minStep;
      k0 = Math.floor(min / step + 1e-9);
      k1 = Math.ceil(max / step - 1e-9);
      if (k1 - k0 <= 6) break;
    }
    if (k1 - k0 < 2) { k0 -= 1; k1 = Math.max(k1 + 1, k0 + 2); }
    const ticks = [];
    for (let k = k0; k <= k1; k++) ticks.push(k * step);
    const decimals = Math.max(0, Math.ceil(-Math.log10(step) - 1e-9));
    return { lo: k0 * step, hi: k1 * step, ticks, decimals };
  }

  // Chart of the station page (FR-8.4 of the station): grid, axes, a line that
  // breaks after `gap` seconds. Returns the scale for the cursor.
  function drawChart(svg, pts, series, digits, gap) {
    const W = svg.clientWidth || 600, H = 170;
    const L = 46, R = 10, T = 8, B = 22;
    svg.setAttribute("viewBox", `0 0 ${W} ${H}`);
    if (pts.length < 2) {
      svg.innerHTML = `<text x="${W / 2}" y="${H / 2}" text-anchor="middle">No data yet</text>`;
      return null;
    }
    const t0 = pts[0][0], t1 = pts[pts.length - 1][0];
    const ax = niceAxis(Math.min(...pts.map((p) => p[1])), Math.max(...pts.map((p) => p[1])), digits);
    const vmin = ax.lo, vmax = ax.hi;
    const x = (t) => L + (t - t0) / Math.max(1, t1 - t0) * (W - L - R);
    const y = (v) => T + (vmax - v) / (vmax - vmin) * (H - T - B);

    let out = "";
    for (const v of ax.ticks) {
      const yy = y(v).toFixed(1);
      out += `<line class="grid" x1="${L}" x2="${W - R}" y1="${yy}" y2="${yy}"/>`;
      out += `<text x="${L - 6}" y="${yy}" dy="4" text-anchor="end">${v.toFixed(ax.decimals)}</text>`;
    }
    const ticks = W < 420 ? 2 : 4;
    const days = t1 - t0 > 2 * 86400;
    for (let i = 0; i <= ticks; i++) {
      const t = t0 + (t1 - t0) * i / ticks, date = new Date(t * 1000);
      const label = days ? date.toLocaleDateString([], { day: "2-digit", month: "2-digit" })
                         : date.toLocaleTimeString([], { hour: "2-digit", minute: "2-digit", hour12: false });
      const anchor = i === 0 ? "start" : i === ticks ? "end" : "middle";
      out += `<text x="${x(t).toFixed(1)}" y="${H - 6}" text-anchor="${anchor}">${label}</text>`;
    }
    let d = "", prev = null;
    for (const [t, v] of pts) {
      d += (prev === null || t - prev > gap ? "M" : "L") + x(t).toFixed(1) + "," + y(v).toFixed(1);
      prev = t;
    }
    out += `<path class="series ${series}" d="${d}"/>`;
    out += `<g class="cursor"></g>`;
    svg.innerHTML = out;
    return { x, y, t0, t1, W, H, L, R, T, B };
  }

  return {
    $, el, now, setServerTime, num, fmt, signed, fmtAge, fmtTime, fmtDuration, mslFactor, signal,
    trendText, trendArrow, stationState, stateBadge, STATE_LABEL, sensorTitle, channelsOf, KNOWN,
    unknownValue, calibration, api, live, keepFresh, showBanner, drawChart,
  };
})();
