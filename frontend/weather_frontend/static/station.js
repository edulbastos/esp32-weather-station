// Station page (FR-4.2): the content of the station's own page, one section per sensor.
"use strict";

(() => {
  const { $, el } = WS;
  const deviceId = decodeURIComponent(location.pathname.split("/").pop());
  const RANGES = { 1: "hour", 24: "24 hours", 168: "7 days" };
  const PERIODS = [1, 24, 168];
  // The three charts of a sensor: channel, payload field, series class, finest axis step (10^-digits), unit.
  const CHARTS = [
    { channel: "temperature", label: "Temperature (°C)", series: "s0", digits: 1, unit: "°C" },
    { channel: "humidity", label: "Humidity (%RH)", series: "s1", digits: 0, unit: "%RH" },
    { channel: "pressure", label: "Pressure (hPa)", series: "s2", digits: 1, unit: "hPa" },
  ];

  let station = null;
  let groupsKey = "";         // sensor sections currently built
  const data = {};            // hours -> { bucket, gap, info, loadedAt, sensors: key -> { points, lastAdded } }
  const scales = {};          // svg id -> scale of the drawn chart
  let cursorT = null;         // FR-8.7 of the station: shared cursor
  let chartsTimer = null, stationTimer = null, chartsGen = 0;

  let hours = 24;
  try {
    const saved = Number(localStorage.getItem("chart-hours"));
    if (RANGES[saved]) hours = saved;
  } catch (e) { /* storage unavailable: keep the default */ }

  // -- data ---------------------------------------------------------------

  async function loadStation() {
    clearTimeout(stationTimer);
    try {
      const d = await WS.api("/api/stations/" + encodeURIComponent(deviceId));
      WS.setServerTime(d.server_time);
      WS.showBanner(d.mqtt_connected);
      $("user").textContent = d.user;
      station = d.station;
      render();
    } catch (e) {
      if (e.status === 404) {
        $("status").textContent = "";
        $("empty").textContent = "Unknown station " + deviceId + ".";
        $("empty").classList.remove("hidden");
        return;
      }
      setStatus("Frontend unreachable, retrying…", true);
      stationTimer = setTimeout(loadStation, 10000);
    }
  }

  async function loadCharts() {
    clearTimeout(chartsTimer);
    const gen = ++chartsGen;
    try {
      const d = await WS.api("/api/stations/" + encodeURIComponent(deviceId) + "/charts");
      if (gen !== chartsGen) return;
      d.periods.forEach(setChartData);
      drawCharts();
    } catch (e) {
      if (gen !== chartsGen) return;
      $("history-info").textContent = "charts could not be loaded, retrying…";
      chartsTimer = setTimeout(loadCharts, 10000);
    }
  }

  function reloadAll() {
    loadStation();
    loadCharts();
  }

  // FR-5.2, FR-5.3: buckets of the backend; the pressure is already at sea level.
  function setChartData(p) {
    const sensors = {};
    for (const s of p.sensors) {
      const pts = s.records.map((x) => ({ t: x[0], v: [x[1], x[2], x[3]], n: 0 }));
      sensors[s.key] = { points: pts, lastAdded: pts.length ? pts[pts.length - 1].t : 0 };
    }
    data[p.hours] = {
      loadedAt: Date.now(),
      bucket: p.bucket_s,
      publish: p.publish_period_s,
      gap: 3 * Math.max(p.bucket_s, p.publish_period_s),  // longer pauses break the line
      info: !p.available ? "no stored readings in this period"
        : "each point is the average of " + WS.fmtDuration(p.bucket_s) + "; readings every " +
          WS.fmtDuration(p.publish_period_s),
      sensors,
    };
  }

  // A new reading goes into every period, averaged into the last point while
  // that point is younger than one bucket (as on the station page).
  function addPoint(key, t, v) {
    const sel = data[hours] && data[hours].sensors[key];
    if (sel && sel.lastAdded > 0 && t > sel.lastAdded) {
      const maxStep = 2 * data[hours].publish + 10;
      if (t - sel.lastAdded > maxStep && Date.now() - data[hours].loadedAt > 60000) {
        loadCharts();  // readings were missed: reload instead of leaving a gap
        return;
      }
    }
    for (const h of PERIODS) {
      const c = data[h];
      if (!c) continue;
      const s = c.sensors[key] || (c.sensors[key] = { points: [], lastAdded: 0 });
      if (t <= s.lastAdded) continue;
      s.lastAdded = t;
      const pts = s.points, last = pts[pts.length - 1];
      if (last && last.n > 0 && t - last.start < c.bucket) {
        last.v = last.v.map((x, i) => (x * last.n + v[i]) / (last.n + 1));
        last.t = (last.t * last.n + t) / (last.n + 1);
        last.n++;
      } else {
        pts.push({ t, v, n: 1, start: t });
      }
      const cutoff = t - h * 3600;
      while (pts.length && pts[0].t < cutoff) pts.shift();
    }
    drawCharts();
  }

  // -- rendering ----------------------------------------------------------

  function setStatus(text, error) {
    $("status").textContent = text;
    $("status").classList.toggle("error", !!error);
  }

  function render() {
    if (!station) return;
    const st = station;
    document.title = (st.name || st.device_id) + " · Weather Station";
    $("title-name").textContent = st.name ? " · " + st.name : "";
    renderHeader();
    const key = st.sensors.map((g) => g.key + ":" + WS.channelsOf(g).map((c) => c.id).join(",")).join("|");
    if (key !== groupsKey) {
      groupsKey = key;
      buildSections();
    }
    st.sensors.forEach(renderValues);
    renderFooter();
    $("empty").classList.toggle("hidden", st.sensors.length > 0);
    if (!st.sensors.length) $("empty").textContent = "This station has not sent a reading yet.";
  }

  function newestReading() {
    let best = null;
    for (const g of station.sensors) {
      if (g.latest && (!best || g.latest.received_at > best.received_at)) best = g.latest;
    }
    return best;
  }

  function renderHeader() {
    const st = station, state = WS.stationState(st);
    const box = $("status");
    box.replaceChildren(WS.stateBadge(state));
    box.classList.remove("error");
    const r = newestReading();
    const age = state === "offline" ? "last seen " + WS.fmtAge(WS.now() - st.last_seen)
      : st.last_reading != null ? "updated " + WS.fmtAge(WS.now() - st.last_reading) : "no reading yet";
    const when = r && r.time && !r.time_from_receipt ? " · measured " + WS.fmtTime(r.time) : "";
    box.append(" · " + age + when);
    $("rssi").textContent = r && WS.num(r.rssi_dbm) ? WS.signal(r.rssi_dbm) : "";
    document.querySelector("main").classList.toggle("greyed", state !== "online");
  }

  function valueCard(label, unit, idBase) {
    const card = el("div", "card");
    const lab = el("div", "label", label);
    lab.dataset.role = idBase + "-label";
    const value = el("div", "value");
    const span = el("span", null, "--.-");
    span.dataset.role = idBase;
    value.append(span, el("span", "unit", unit));
    card.append(lab, value);
    return card;
  }

  function sub(card, role) {
    const s = el("div", "sub");
    s.dataset.role = role;
    card.append(s);
  }

  // One section per sensor: value cards, period buttons, charts (FR-4.2).
  function buildSections() {
    const root = $("sensors");
    root.replaceChildren();
    const many = station.sensors.length > 1;
    for (const g of station.sensors) {
      const sec = el("section", "sensor-section");
      sec.dataset.key = g.key;
      if (many) {
        const h = el("h2", "sensor-title", g.sensor_id);
        h.append(el("span", "label", "  " + WS.sensorTitle(g).slice(g.sensor_id.length)));
        sec.append(h);
      }
      const cards = el("div", "cards");
      const channels = WS.channelsOf(g);
      for (const ch of channels) {
        if (ch.id === "temperature") {
          cards.append(valueCard("Temperature", "°C", "temp"));
        } else if (ch.id === "humidity") {
          const c = valueCard("Humidity", "%RH", "hum");
          sub(c, "dew");
          cards.append(c);
        } else if (ch.id === "pressure") {
          const c = valueCard("Pressure", "hPa", "pres");
          sub(c, "pres-station");
          sub(c, "pres-trend");
          cards.append(c);
          const f = el("div", "card");
          const ft = el("div", "value-text", "–");
          ft.dataset.role = "forecast";
          f.append(el("div", "label", "Forecast"), ft);
          sub(f, "forecast-note");
          cards.append(f);
        } else {
          const c = valueCard(ch.id, ch.unit || "", "ch-" + ch.id);
          cards.append(c);
        }
      }
      sec.append(cards);

      const charts = CHARTS.filter((c) => channels.some((ch) => ch.id === c.channel));
      if (charts.length) {
        const head = el("div", "chart-head");
        const h2 = el("h2", null, "Last ");
        const lab = el("span", "range-label", RANGES[hours]);
        h2.append(lab);
        const ranges = el("div", "ranges");
        ranges.setAttribute("role", "group");
        ranges.setAttribute("aria-label", "Chart period");
        for (const [h, text] of [[1, "1 h"], [24, "24 h"], [168, "7 d"]]) {
          const b = el("button", null, text);
          b.type = "button";
          b.dataset.hours = h;
          b.setAttribute("aria-pressed", String(h === hours));
          b.addEventListener("click", () => selectRange(h));
          ranges.append(b);
        }
        head.append(h2, ranges);
        const box = el("section", "charts");
        charts.forEach((c) => {
          const card = el("div", "card chart");
          const label = el("div", "label", c.label);
          label.dataset.role = "chart-label-" + c.channel;
          const svg = document.createElementNS("http://www.w3.org/2000/svg", "svg");
          svg.id = "chart-" + g.key + "-" + c.channel;
          svg.addEventListener("pointermove", moveCursor);
          svg.addEventListener("pointerdown", moveCursor);
          svg.addEventListener("pointerleave", (e) => { if (e.pointerType === "mouse") hideCursor(); });
          card.append(label, svg);
          box.append(card);
        });
        sec.append(head, box);
      }
      root.append(sec);
    }
    drawCharts();
  }

  function role(sec, name) {
    return sec.querySelector(`[data-role="${name}"]`);
  }

  function setText(node, text) {
    if (node) node.textContent = text;
  }

  function renderValues(g) {
    const sec = document.querySelector(`.sensor-section[data-key="${CSS.escape(g.key)}"]`);
    if (!sec) return;
    const r = g.latest || {};
    const alt = station.altitude_m || 0;
    setText(role(sec, "temp"), WS.fmt(r.temperature_c));
    setText(role(sec, "hum"), WS.fmt(r.humidity_rh));
    setText(role(sec, "dew"), WS.num(r.dew_point_c) ? "dew point " + WS.fmt(r.dew_point_c) + " °C" : "");
    // Station FR-11.2: sea-level pressure as the main value when an altitude is set.
    const msl = WS.num(r.pressure_msl_hpa) ? r.pressure_msl_hpa
      : WS.num(r.pressure_hpa) ? r.pressure_hpa * WS.mslFactor(alt) : null;
    setText(role(sec, "pres"), WS.fmt(msl));
    setText(role(sec, "pres-label"), alt ? "Pressure (sea level)" : "Pressure");
    setText(role(sec, "chart-label-pressure"), alt ? "Pressure, sea level (hPa)" : "Pressure (hPa)");
    setText(role(sec, "pres-station"), alt && WS.num(r.pressure_hpa) ? "station " + WS.fmt(r.pressure_hpa) + " hPa" : "");
    setText(role(sec, "pres-trend"), WS.trendText(r));
    // Station FR-13.2.
    if (r.forecast) {
      setText(role(sec, "forecast"), r.forecast);
      setText(role(sec, "forecast-note"), "Zambretti " + r.forecast_code + ", from pressure and tendency");
    } else {
      setText(role(sec, "forecast"), "–");
      setText(role(sec, "forecast-note"), g.latest ? "needs 3 hours of measurements" : "");
    }
    for (const ch of WS.channelsOf(g)) {
      if (!WS.KNOWN[ch.id]) setText(role(sec, "ch-" + ch.id), WS.fmt(WS.unknownValue(r, ch)));
    }
  }

  function renderFooter() {
    const st = station;
    $("footer").classList.remove("hidden");
    $("csv").href = "/stations/" + encodeURIComponent(st.device_id) + "/history.csv";
    $("csv").download = "history-" + st.device_id + ".csv";
    const sys = [];
    if (st.firmware) sys.push("firmware " + st.firmware);
    if (st.measure_period_s) {
      sys.push("measure " + WS.fmtDuration(st.measure_period_s) + ", publish " + WS.fmtDuration(st.publish_period_s) +
        ", log " + WS.fmtDuration(st.log_period_s));
    }
    if (st.altitude_m != null) sys.push("altitude " + st.altitude_m + " m");
    $("system").textContent = sys.join(" · ");

    // Station FR-10.5, FR-11.1: name, identifiers, calibration.
    const line = $("station");
    line.replaceChildren((st.name ? st.name + " · " : "") + "device " + st.device_id);
    for (const g of st.sensors) {
      line.append(" · sensor " + g.sensor_id);
      const cal = WS.calibration(g);
      if (cal) {
        const tag = el("span", null, " (calibrated)");
        tag.title = cal;
        line.append(tag);
      }
    }

    // FR-4.6: METAR check of station FR-11.4.
    const m = st.metar;
    if (!m) {
      $("metar").textContent = "–";
      $("metar").title = "";
    } else {
      let text = m.station + ", observed " + WS.fmtTime(m.obs_time) + ": airport " + WS.fmt(m.qnh_ref_hpa) +
        " hPa, station " + WS.fmt(m.qnh_hpa) + " hPa, difference " + WS.signed(m.diff_hpa) + " hPa";
      text += m.median_diff_hpa != null
        ? " · median " + WS.signed(m.median_diff_hpa) + " hPa of " + m.samples + " samples · to cancel it: cal set pressure " +
          WS.fmt(m.suggested_offset_hpa) + ", or altitude " + m.suggested_altitude_m + " m"
        : " · " + m.samples + " samples, median from 24";
      $("metar").textContent = text;
      $("metar").title = "Station QNH minus airport QNH";
    }

    const h = st.history || {};
    $("stored").textContent = h.readings
      ? "stored by the frontend: " + h.readings.toLocaleString() + " readings since " + WS.fmtTime(h.first)
      : "no readings stored yet";
    const changes = st.sensor_changes || [];
    $("changes-row").classList.toggle("hidden", !changes.length);
    $("changes").textContent = changes.map((c) =>
      "position " + c.index + " replaced " + WS.fmtTime(c.time) + ": " + c.old_id + " → " + c.new_id).join(" · ");
  }

  // -- charts -------------------------------------------------------------

  function drawCharts() {
    const chart = data[hours];
    if (!chart || !station) {
      if (!chart) $("history-info").textContent = "loading…";
      return;
    }
    $("history-info").textContent = chart.info;
    for (const g of station.sensors) {
      const s = chart.sensors[g.key] || { points: [] };
      CHARTS.forEach((c, i) => {
        const svg = $("chart-" + g.key + "-" + c.channel);
        if (!svg) return;
        const pts = s.points.filter((p) => WS.num(p.v[i])).map((p) => [p.t, p.v[i]]);
        scales[svg.id] = WS.drawChart(svg, pts, c.series, c.digits, chart.gap);
      });
    }
    drawCursor();
  }

  function showLoading() {
    document.querySelectorAll(".chart svg").forEach((svg) => {
      const W = svg.clientWidth || 600;
      scales[svg.id] = null;
      svg.setAttribute("viewBox", `0 0 ${W} 170`);
      svg.innerHTML = `<text x="${W / 2}" y="85" text-anchor="middle">Loading…</text>`;
    });
    $("history-info").textContent = "loading…";
  }

  function nearestPoint(pts, t) {
    let lo = 0, hi = pts.length - 1;
    if (hi < 0) return null;
    while (hi - lo > 1) {
      const mid = (lo + hi) >> 1;
      if (pts[mid].t < t) lo = mid; else hi = mid;
    }
    return Math.abs(pts[lo].t - t) <= Math.abs(pts[hi].t - t) ? pts[lo] : pts[hi];
  }

  function fmtCursorTime(t) {
    const date = new Date(t * 1000);
    if (hours === 168) {
      return date.toLocaleString([], { day: "2-digit", month: "2-digit", hour: "2-digit", minute: "2-digit", hour12: false });
    }
    return date.toLocaleTimeString([], { hour: "2-digit", minute: "2-digit", second: hours === 1 ? "2-digit" : undefined,
                                         hour12: false });
  }

  // One cursor for every chart of the page, each at its own nearest point.
  function drawCursor() {
    const chart = data[hours];
    if (!chart || !station) return;
    for (const g of station.sensors) {
      const s = chart.sensors[g.key];
      let p = null;
      if (s && cursorT !== null) {
        p = nearestPoint(s.points, cursorT);
        if (p && Math.abs(p.t - cursorT) > chart.gap / 2) p = null;
      }
      CHARTS.forEach((c, i) => {
        const svg = $("chart-" + g.key + "-" + c.channel);
        if (!svg) return;
        const sc = scales[svg.id], grp = svg.querySelector(".cursor");
        if (!grp) return;
        if (!p || !sc || !WS.num(p.v[i])) {
          grp.innerHTML = "";
          return;
        }
        const xx = sc.x(p.t), yy = sc.y(p.v[i]);
        const left = xx > (sc.L + sc.W - sc.R) / 2;
        const label = `${p.v[i].toFixed(2)} ${c.unit} · ${fmtCursorTime(p.t)}`;
        grp.innerHTML =
          `<line x1="${xx.toFixed(1)}" x2="${xx.toFixed(1)}" y1="${sc.T}" y2="${sc.H - sc.B}"/>` +
          `<circle class="${c.series}" cx="${xx.toFixed(1)}" cy="${yy.toFixed(1)}" r="4"/>` +
          `<text x="${(left ? xx - 6 : xx + 6).toFixed(1)}" y="${sc.T + 10}" ` +
          `text-anchor="${left ? "end" : "start"}">${label}</text>`;
      });
    }
  }

  function moveCursor(e) {
    const sc = scales[e.currentTarget.id];
    if (!sc) return;
    const r = e.currentTarget.getBoundingClientRect();
    const px = (e.clientX - r.left) * sc.W / r.width;
    const f = Math.min(1, Math.max(0, (px - sc.L) / (sc.W - sc.L - sc.R)));
    cursorT = sc.t0 + f * (sc.t1 - sc.t0);
    drawCursor();
  }

  function hideCursor() {
    if (cursorT === null) return;
    cursorT = null;
    drawCursor();
  }

  document.addEventListener("pointerdown", (e) => {
    if (!e.target.closest || !e.target.closest(".chart svg")) hideCursor();
  });

  // The period buttons of every sensor section change all charts together.
  function selectRange(h) {
    hours = h;
    cursorT = null;
    try { localStorage.setItem("chart-hours", String(h)); } catch (e) { /* not essential */ }
    document.querySelectorAll(".range-label").forEach((n) => { n.textContent = RANGES[h]; });
    document.querySelectorAll(".ranges button").forEach((b) => {
      b.setAttribute("aria-pressed", String(Number(b.dataset.hours) === h));
    });
    if (data[h]) drawCharts(); else showLoading();
  }

  let resizeTimer;
  window.addEventListener("resize", () => {
    clearTimeout(resizeTimer);
    resizeTimer = setTimeout(drawCharts, 150);
  });

  // -- live updates (FR-4.3) ------------------------------------------------------

  function onEvent(type, ev) {
    if (ev.device_id !== deviceId || !station) return;
    if (type === "reading") {
      const g = station.sensors.find((x) => x.sensor_ids.includes(ev.sensor_id));
      if (!g) return loadStation();  // a new sensor
      const r = ev.reading;
      if (g.sensor_id === ev.sensor_id || !g.latest || r.time >= g.latest.time) g.latest = r;
      station.last_reading = r.received_at;
      station.last_seen = r.received_at;
      renderHeader();
      renderValues(g);
      if (!r.time_from_receipt) {
        const msl = WS.num(r.pressure_msl_hpa) ? r.pressure_msl_hpa
          : r.pressure_hpa * WS.mslFactor(station.altitude_m || 0);
        addPoint(g.key, r.time, [r.temperature_c, r.humidity_rh, msl]);
      }
    } else if (type === "status") {
      station.status = ev.status;
      station.status_time = ev.time;
      renderHeader();
    } else {
      clearTimeout(stationTimer);
      stationTimer = setTimeout(loadStation, 1000);  // info or METAR: reload the station
    }
  }

  WS.live({
    event: onEvent,
    reload: reloadAll,
    mqtt: WS.showBanner,
    down: () => setStatus("Live updates interrupted, reconnecting…", true),
  });
  WS.keepFresh(reloadAll);
  setInterval(() => { if (station) renderHeader(); }, 5000);
  selectRange(hours);
  reloadAll();
})();
