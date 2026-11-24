"use strict";

// The live map (ADR 0030): only real plans, along the roads. The best one is
// bright; the plans it replaced stay on the map, dimmer the older they are.

if (!window.maplibregl || !window.deck) {
  const empty = document.getElementById("empty");
  empty.querySelector("h1").textContent = "The map could not load";
  empty.querySelector("p").textContent =
    "Its libraries come from cdn.jsdelivr.net, which this browser cannot reach.";
  for (const message of [
    { type: "streamlit:componentReady", apiVersion: 1 },
    { type: "streamlit:setFrameHeight", height: 820 },
  ]) {
    window.parent.postMessage({ isStreamlitMessage: true, ...message }, "*");
  }
  throw new Error("the map libraries did not load");
}

const BASEMAPS = {
  dark: "https://tiles.openfreemap.org/styles/dark",
  light: "https://tiles.openfreemap.org/styles/positron",
};
const plainBasemap = (theme) => ({
  version: 8,
  sources: {},
  layers: [
    {
      id: "page",
      type: "background",
      paint: { "background-color": theme === "dark" ? "#05070e" : "#eef2f8" },
    },
  ],
});
const ISLAND_COLORS = {
  dark: ["#22e6ff", "#ff2bd6", "#7dff5a", "#ffc02e", "#9d6bff", "#ff5d5d", "#2bffc3", "#ff8f1f"],
  light: ["#0077b6", "#c2187f", "#2a8a12", "#b86e00", "#5f35d6", "#d02c2c", "#00866b", "#c85a00"],
};
const PHASES = [
  ["queued", "Queued"],
  ["prepare", "Snapping stops"],
  ["distance", "Road distances"],
  ["solve", "Searching"],
  ["finalize", "Road routing"],
  ["done", "Done"],
];
const MAX_FRAMES = 60;
const MAX_POINTS = 2_000_000; // kept for replay, across all plans
const OLD_PLANS = 6; // shown behind the best
const OLD_OPACITY = { dark: 0.34, light: 0.4 };
const OLD_FADE = 0.62; // each older plan, relative to the next
const DIM_MS = 900; // a replaced plan dims over this long
const REVEAL_MS = 1800; // the first plan draws itself along the roads
const PULSE_MS = 7000;
const FIT_PADDING = { top: 84, right: 356, bottom: 226, left: 40 };
const FIT_PADDING_NARROW = { top: 120, right: 24, bottom: 150, left: 24 };
const CALM = window.matchMedia("(prefers-reduced-motion: reduce)").matches;

const $ = (id) => document.getElementById(id);

const state = {
  view: null,
  theme: "dark",
  job: null,
  stopsKey: "",
  frames: [], // each best plan on the roads, oldest first
  nextFrame: 0,
  islands: new Map(), // island id -> best cost
  finalSeen: false,
  replay: null, // the frame being replayed, or null when live
  layers: { best: true, trails: true },
  shownCost: null,
  bestCost: null, // the lowest live cost, to pulse when it drops
  heldPlan: null, // the cost of the plan the app knows this map has
  customers: [],
  customersKey: "",
  clockBase: 0,
  clockAt: 0,
};

// ---- Geometry and colour -----------------------------------------------------

function hslToRgb(hue, saturation, lightness) {
  const a = saturation * Math.min(lightness, 1 - lightness);
  const channel = (n) => {
    const k = (n + hue / 30) % 12;
    return Math.round(255 * (lightness - a * Math.max(-1, Math.min(k - 3, 9 - k, 1))));
  };
  return [channel(0), channel(8), channel(4)];
}

// Coloured by where it heads from the depot, so that a route keeps its colour
// from one plan to the next, however the solver numbers it.
function routeColor(path) {
  const [x0, y0] = path[0];
  let x = 0;
  let y = 0;
  for (const point of path) {
    x += point[0] - x0;
    y += point[1] - y0;
  }
  const hue = ((Math.atan2(y, x) * 180) / Math.PI + 360) % 360;
  return state.theme === "dark" ? hslToRgb(hue, 1, 0.62) : hslToRgb(hue, 0.86, 0.38);
}

// Each vertex's share of the way along the path, so that pulses keep a speed.
function timestamps(path) {
  const distances = [0];
  for (let i = 1; i < path.length; i += 1) {
    const [x0, y0] = path[i - 1];
    const [x1, y1] = path[i];
    const dx = (x1 - x0) * Math.cos((y0 * Math.PI) / 180);
    distances.push(distances[i - 1] + Math.hypot(dx, y1 - y0));
  }
  const total = distances[distances.length - 1] || 1;
  return distances.map((d) => d / total);
}

function makeRoute(path, stops, cost, index) {
  return { path, stops, cost, index, color: routeColor(path), times: timestamps(path) };
}

// A live plan's route: points in 1e-7 degrees, as differences.
function decodeRoute(route, index) {
  let latitude = 0;
  let longitude = 0;
  const path = route.latitudes.map((step, i) => {
    latitude += step;
    longitude += route.longitudes[i];
    return [longitude / 1e7, latitude / 1e7];
  });
  // A route whose stops all sit on the depot's node has one point.
  return makeRoute(path.length === 1 ? [path[0], path[0]] : path, route.stops, route.cost, index);
}

// ---- Formatting --------------------------------------------------------------

function formatCost(cost, edgeCost) {
  if (cost === null || cost === undefined) {
    return "—";
  }
  if (edgeCost === "time") {
    const minutes = Math.round(cost / 600);
    return minutes < 60
      ? `${minutes} min`
      : `${Math.floor(minutes / 60)} h ${String(minutes % 60).padStart(2, "0")} min`;
  }
  const kilometres = cost / 1000;
  return `${kilometres.toLocaleString("en", {
    minimumFractionDigits: kilometres < 1000 ? 2 : 0,
    maximumFractionDigits: kilometres < 1000 ? 2 : 0,
  })} km`;
}

function formatClock(seconds) {
  const whole = Math.max(0, Math.floor(seconds));
  const minutes = Math.floor(whole / 60);
  return `${minutes}:${String(whole % 60).padStart(2, "0")}`;
}

// ---- Taking in what Streamlit sends ------------------------------------------

function reset(job) {
  Object.assign(state, {
    job,
    frames: [],
    islands: new Map(),
    finalSeen: false,
    replay: null,
    shownCost: null,
    bestCost: null,
    heldPlan: null,
  });
}

function addFrame(view, cost, routes, now) {
  const points = view.points || [];
  const seconds = points.length ? points[points.length - 1][0] : 0;
  const size = routes.reduce((sum, route) => sum + route.path.length, 0);
  state.nextFrame += 1;
  state.frames.push({ id: state.nextFrame, cost, seconds, routes, size, at: now });
  const kept = state.frames.reduce((sum, frame) => sum + frame.size, 0);
  const recent = state.frames.length - 1 - OLD_PLANS;
  if (recent > 1 && (state.frames.length > MAX_FRAMES || kept > MAX_POINTS)) {
    // Keeps the plans on show and the first, and every other one between.
    state.frames = state.frames.filter((_, i) => i >= recent || i % 2 === 0);
    state.replay = null;
  }
}

function takeSolutions(view, now) {
  state.islands = new Map((view.islands || []).map((island) => [island.id, island.cost]));
  const newest = () => state.frames[state.frames.length - 1];
  if (view.plan && (!newest() || view.plan.cost < newest().cost)) {
    addFrame(view, view.plan.cost, view.plan.routes.map(decodeRoute), now);
  }
  if (view.plan && view.plan.cost !== state.heldPlan) {
    // So that the app stops sending this large plan with every update.
    state.heldPlan = view.plan.cost;
    post("streamlit:setComponentValue", {
      value: { job: view.job, cost: view.plan.cost },
      dataType: "json",
    });
  }
  if (view.result && !state.finalSeen) {
    state.finalSeen = true;
    const routes = view.result.routes.map((route, index) =>
      makeRoute(route.polyline, route.stops, route.cost, index),
    );
    if (newest()?.cost === view.result.cost) {
      newest().routes = routes; // the same plan, with every road point
    } else {
      addFrame(view, view.result.cost, routes, now);
    }
  }
}

function recolor() {
  const paint = (route) => {
    route.color = routeColor(route.path);
  };
  state.frames.forEach((frame) => frame.routes.forEach(paint));
}

function applyTheme(base) {
  const theme = base === "light" ? "light" : "dark";
  if (theme === state.theme && document.documentElement.dataset.theme) {
    return;
  }
  state.theme = theme;
  document.documentElement.dataset.theme = theme;
  recolor();
  map.setStyle(BASEMAPS[theme]);
  drawChart();
}

function fitToStops(view) {
  const points = view.stops?.length
    ? view.stops
    : view.result?.routes.flatMap((route) => route.polyline) ?? [];
  // A job keeps its stops; a problem being edited moves them.
  const key = view.job ? `${view.job}:${points.length}` : JSON.stringify(points);
  if (!points.length || key === state.stopsKey) {
    return;
  }
  state.stopsKey = key;
  const bounds = new maplibregl.LngLatBounds(points[0], points[0]);
  points.forEach((point) => bounds.extend(point));
  map.fitBounds(bounds, {
    padding: window.innerWidth < 860 ? FIT_PADDING_NARROW : FIT_PADDING,
    duration: CALM ? 0 : 1800,
    maxZoom: 15,
  });
}

function render(view, theme) {
  applyTheme(theme?.base ?? (matchMedia("(prefers-color-scheme: light)").matches ? "light" : "dark"));
  if ((view.job ?? null) !== state.job) {
    reset(view.job ?? null);
  }
  state.view = view;
  state.clockBase = view.elapsed ?? 0;
  state.clockAt = performance.now();
  takeSolutions(view, performance.now());
  fitToStops(view);
  updatePanels();
  drawChart();
}

// ---- The panels --------------------------------------------------------------

function updatePhases(view) {
  const phase = view.phase;
  const ended = phase === "failed" || phase === "cancelled";
  const reached = ended ? PHASES.length : PHASES.findIndex(([name]) => name === phase);
  const items = PHASES.filter(([name]) => !(ended && name === "done")).map(([name, label], index) => {
    const item = document.createElement("li");
    item.textContent = label;
    if (phase === "done" || index < reached) {
      item.className = ended ? "" : "past";
    } else if (index === reached) {
      item.className = "now";
    }
    return item;
  });
  if (ended) {
    const item = document.createElement("li");
    item.className = "failed";
    item.textContent = phase === "failed" ? "Failed" : "Cancelled";
    items.push(item);
  }
  $("phases").replaceChildren(...items);
}

function stat(term, value) {
  const dt = document.createElement("dt");
  dt.textContent = term;
  const dd = document.createElement("dd");
  dd.textContent = value;
  const group = document.createElement("div");
  group.append(dt, dd);
  return group;
}

function shownFrame() {
  return state.replay === null ? state.frames[state.frames.length - 1] : state.frames[state.replay];
}

// Only what islands have reported: a road plan's worker can still fail first.
function headlineCost(view, frame) {
  if (state.replay !== null) {
    return frame.cost;
  }
  if (view?.result) {
    return view.result.cost;
  }
  return state.islands.size ? Math.min(...state.islands.values()) : null;
}

function updateScore(view) {
  const frame = shownFrame();
  const cost = headlineCost(view, frame);
  // From the stored history, so that a reloaded page shows the same numbers.
  const points = view.points ?? [];
  const first = points[0]?.[1];
  const routes = frame?.routes;
  $("score-label").textContent =
    state.replay !== null ? "Replaying plan" : view.phase === "done" ? "Final plan" : "Best plan";
  $("gain").textContent =
    cost !== null && first > cost
      ? `−${(((first - cost) / first) * 100).toFixed(1)}% since the first plan`
      : "";
  // A job reopened after a client restart has only its result's stops.
  const customers = view.stops?.length
    ? view.stops.length - 1
    : (view.result?.routes ?? []).reduce((sum, route) => sum + route.stops.length, 0);
  const costs = (routes ?? []).map((route) => route.cost);
  const edgeCost = view.edgeCost;
  $("stats").replaceChildren(
    stat("Vehicles used", routes ? `${routes.length}${view.vehicles ? ` / ${view.vehicles}` : ""}` : "—"),
    stat("Customers", customers ? customers.toLocaleString("en") : "—"),
    stat("Max per vehicle", costs.length ? formatCost(Math.max(...costs), edgeCost) : "—"),
    stat("Min per vehicle", costs.length ? formatCost(Math.min(...costs), edgeCost) : "—"),
    stat("Improvements", points.length ? points.length.toLocaleString("en") : "—"),
    stat("Elapsed", formatClock(state.clockBase)),
  );
  $("stats").lastChild.lastChild.id = "clock";
}

function updateIslands(view) {
  const islands = [...state.islands.entries()].sort((a, b) => a[1] - b[1]);
  $("islands").hidden = islands.length === 0;
  if (!islands.length) {
    return;
  }
  $("islands-label").textContent = view.running ? "Islands searching" : "Island results";
  const best = islands[0][1];
  const worst = islands[islands.length - 1][1];
  const colors = ISLAND_COLORS[state.theme];
  $("island-list").replaceChildren(
    ...islands.map(([id, islandCost], rank) => {
      const item = document.createElement("li");
      item.style.setProperty("--hue", colors[id % colors.length]);
      const share = worst > best ? 100 - 55 * ((islandCost - best) / (worst - best)) : 100;
      item.style.setProperty("--share", `${share}%`);
      const name = document.createElement("span");
      name.textContent = `Island ${id}`;
      if (rank === 0) {
        const tag = document.createElement("em");
        tag.textContent = "best";
        name.append(tag);
      }
      const cost = document.createElement("b");
      cost.textContent = formatCost(islandCost, view.edgeCost);
      item.append(document.createElement("i"), name, cost, document.createElement("u"));
      return item;
    }),
  );
}

function updateNotice(view) {
  let searching = "";
  if (view.phase === "solve" && !state.frames.length) {
    searching = state.islands.size
      ? "Tracing the best plan along the roads…"
      : "The islands are looking for a first feasible plan…";
  }
  const waiting = view.running && view.workers === 0;
  const text =
    view.failure ||
    view.warning ||
    (waiting ? "No worker is connected, so this job is waiting for one." : searching);
  $("notice").hidden = !text;
  $("notice").textContent = text;
  $("notice").classList.toggle("soft", !view.failure);
}

function updatePanels() {
  const view = state.view;
  const idle = view.phase === "idle";
  $("empty").hidden = !idle || view.stops?.length > 0;
  $("phases").hidden = idle;
  $("score").hidden = idle;
  $("timeline").hidden = idle || !(view.points?.length > 0);
  $("live").hidden = state.replay === null;
  if (idle) {
    $("islands").hidden = true;
    $("notice").hidden = true;
    return;
  }
  updatePhases(view);
  updateScore(view);
  updateIslands(view);
  updateNotice(view);
}

// ---- The convergence chart ---------------------------------------------------

function chartPoints() {
  const points = state.view?.points ?? [];
  return points.map(([seconds, cost]) => ({ seconds, cost }));
}

function chartScale(points, width, height) {
  const costs = points.map((p) => p.cost);
  const low = Math.min(...costs);
  const high = Math.max(...costs);
  const spread = high - low || Math.max(high * 0.01, 1);
  const end = Math.max(points[points.length - 1].seconds, state.clockBase, 1);
  return {
    x: (seconds) => 4 + (seconds / end) * (width - 8),
    // Compressed at the top: the first plans are far worse than the rest,
    // and would flatten every later improvement.
    // A road plan can arrive before the report of its cost.
    y: (cost) => 10 + (1 - (Math.max(0, cost - low) / spread) ** 0.4) * (height - 26),
    seconds: (x) => ((x - 4) / (width - 8)) * end,
    end,
    low,
    high,
  };
}

function drawChart() {
  const canvas = $("chart");
  const width = canvas.clientWidth;
  const height = canvas.clientHeight;
  const points = chartPoints();
  if (!width || !height || !points.length) {
    return;
  }
  const ratio = window.devicePixelRatio || 1;
  canvas.width = Math.round(width * ratio);
  canvas.height = Math.round(height * ratio);
  const context = canvas.getContext("2d");
  context.scale(ratio, ratio);
  context.clearRect(0, 0, width, height);
  const style = getComputedStyle(document.documentElement);
  const accent = style.getPropertyValue("--accent").trim();
  const accent2 = style.getPropertyValue("--accent-2").trim();
  const muted = style.getPropertyValue("--muted").trim();
  const scale = chartScale(points, width, height);

  const lastCost = points[points.length - 1].cost;
  const line = new Path2D();
  line.moveTo(scale.x(points[0].seconds), scale.y(points[0].cost));
  for (let i = 1; i < points.length; i += 1) {
    line.lineTo(scale.x(points[i].seconds), scale.y(points[i - 1].cost));
    line.lineTo(scale.x(points[i].seconds), scale.y(points[i].cost));
  }
  line.lineTo(scale.x(scale.end), scale.y(lastCost));

  const area = new Path2D(line);
  area.lineTo(scale.x(scale.end), height - 14);
  area.lineTo(scale.x(points[0].seconds), height - 14);
  area.closePath();
  const fill = context.createLinearGradient(0, 0, 0, height);
  fill.addColorStop(0, `color-mix(in srgb, ${accent} 30%, transparent)`);
  fill.addColorStop(1, "transparent");
  context.fillStyle = fill;
  context.fill(area);

  const stroke = context.createLinearGradient(0, 0, width, 0);
  stroke.addColorStop(0, accent2);
  stroke.addColorStop(1, accent);
  context.strokeStyle = stroke;
  context.lineWidth = 2;
  context.lineJoin = "round";
  if (state.theme === "dark") {
    context.shadowColor = accent;
    context.shadowBlur = 10;
  }
  context.stroke(line);
  context.shadowBlur = 0;

  context.fillStyle = muted;
  context.font = "10.5px Inter, 'Segoe UI', system-ui, sans-serif";
  context.textAlign = "left";
  context.fillText("0:00", 4, height - 2);
  context.fillText(formatCost(scale.high, state.view.edgeCost), 4, 9);
  context.textAlign = "right";
  context.fillText(formatClock(scale.end), width - 4, height - 2);
  if (scale.high > scale.low) {
    context.fillText(formatCost(scale.low, state.view.edgeCost), width - 12, scale.y(scale.low) - 8);
  }

  const frame = shownFrame();
  if (frame) {
    const x = scale.x(state.replay === null ? scale.end : frame.seconds);
    const y = scale.y(frame.cost);
    context.strokeStyle = muted;
    context.globalAlpha = 0.5;
    context.setLineDash([3, 3]);
    context.lineWidth = 1;
    context.beginPath();
    context.moveTo(x, 4);
    context.lineTo(x, height - 14);
    context.stroke();
    context.setLineDash([]);
    context.globalAlpha = 1;
    context.fillStyle = accent;
    context.beginPath();
    context.arc(x, y, 4, 0, Math.PI * 2);
    context.fill();
  }
  $("timeline-label").textContent =
    state.replay !== null
      ? `Plan ${state.replay + 1} of ${state.frames.length}`
      : state.frames.length > 1
        ? "Convergence · drag to replay"
        : "Convergence";
}

function scrub(event) {
  const points = chartPoints();
  if (!points.length || state.frames.length < 2) {
    return;
  }
  const canvas = $("chart");
  const bounds = canvas.getBoundingClientRect();
  const scale = chartScale(points, bounds.width, bounds.height);
  const seconds = scale.seconds(event.clientX - bounds.left);
  let index = 0;
  state.frames.forEach((frame, i) => {
    if (frame.seconds <= seconds) {
      index = i;
    }
  });
  state.replay = index;
  updatePanels();
  drawChart();
}

// ---- The layers --------------------------------------------------------------

const OVER = { depthCompare: "always", depthWriteEnabled: false };
// Additive: where neon lines cross, they get brighter.
const ADD = {
  ...OVER,
  blend: true,
  blendColorOperation: "add",
  blendColorSrcFactor: "src-alpha",
  blendColorDstFactor: "one",
  blendAlphaOperation: "add",
  blendAlphaSrcFactor: "one",
  blendAlphaDstFactor: "one-minus-src-alpha",
};
const EVEN = {
  ...OVER,
  blend: true,
  blendColorOperation: "max",
  blendColorSrcFactor: "one",
  blendColorDstFactor: "one",
  blendAlphaOperation: "max",
  blendAlphaSrcFactor: "one",
  blendAlphaDstFactor: "one",
};

// A road's short segments overlap at every joint. With `EVEN`, which needs
// premultiplied colours, a translucent line stays the same strength throughout.
class EvenPathLayer extends deck.PathLayer {
  getShaders() {
    const shaders = super.getShaders();
    const inject = { ...shaders.inject, "fs:DECKGL_FILTER_COLOR": "color.rgb *= color.a;" };
    return { ...shaders, inject };
  }
}
EvenPathLayer.layerName = "EvenPathLayer";

function pathLayer(id, data, options, Layer = deck.PathLayer) {
  return new Layer({
    id,
    data,
    getPath: (route) => route.path,
    getColor: (route) => route.color,
    widthUnits: "pixels",
    capRounded: true,
    jointRounded: true,
    parameters: OVER,
    updateTriggers: { getColor: state.theme },
    ...options,
  });
}

function evenLayer(id, data, options) {
  return pathLayer(id, data, { ...options, parameters: EVEN }, EvenPathLayer);
}

// The plans before the shown one: the newest of them dims as it is replaced.
function oldPlanLayers(index, now) {
  if (!state.layers.trails || index < 1) {
    return [];
  }
  const dimming = state.replay === null ? Math.min(1, (now - state.frames[index].at) / DIM_MS) : 1;
  const layers = [];
  for (let age = Math.min(OLD_PLANS, index); age >= 1; age -= 1) {
    const frame = state.frames[index - age];
    const settled = OLD_OPACITY[state.theme] * OLD_FADE ** (age - 1);
    const opacity = age === 1 ? 0.9 + (settled - 0.9) * (1 - (1 - dimming) ** 2) : settled;
    layers.push(
      evenLayer(`old-${frame.id}-${state.theme}`, frame.routes, {
        getWidth: age === 1 ? 1.8 : 1.3,
        opacity,
      }),
    );
  }
  return layers;
}

function planLayers(frame, now) {
  if (!state.layers.best || !frame) {
    return [];
  }
  const routes = frame.routes;
  const key = `${frame.id}-${state.theme}`;
  const dark = state.theme === "dark";
  const age = state.replay === null ? now - frame.at : Infinity;
  if (state.frames[0] === frame && age < REVEAL_MS && !CALM) {
    return [
      new deck.TripsLayer({
        id: `reveal-${key}`,
        data: routes,
        getPath: (route) => route.path,
        getTimestamps: (route) => route.times,
        getColor: (route) => route.color,
        getWidth: 3,
        widthUnits: "pixels",
        capRounded: true,
        jointRounded: true,
        fadeTrail: false,
        trailLength: 1.01,
        currentTime: 1 - (1 - age / REVEAL_MS) ** 3,
        parameters: OVER,
      }),
    ];
  }
  const layers = [];
  if (dark) {
    layers.push(evenLayer(`glow-wide-${key}`, routes, { getWidth: 11, opacity: 0.13 }));
    layers.push(evenLayer(`glow-${key}`, routes, { getWidth: 5, opacity: 0.32 }));
  }
  layers.push(
    pathLayer(`core-${key}`, routes, {
      getWidth: dark ? 2.1 : 2.8,
      opacity: 0.97,
      pickable: true,
      autoHighlight: true,
      highlightColor: dark ? [255, 255, 255, 120] : [0, 0, 0, 50],
    }),
  );
  if (!CALM) {
    layers.push(
      new deck.TripsLayer({
        id: `pulse-${key}`,
        data: routes,
        getPath: (route) => route.path,
        getTimestamps: (route) => route.times,
        getColor: dark ? [255, 255, 255] : (route) => route.color,
        getWidth: dark ? 3.4 : 4.2,
        widthUnits: "pixels",
        capRounded: true,
        jointRounded: true,
        trailLength: 0.11,
        currentTime: ((now / PULSE_MS) % 1) * 1.14,
        opacity: dark ? 0.9 : 0.75,
        parameters: dark ? ADD : OVER,
      }),
    );
  }
  return layers;
}

function stopLayers(view, routes, key, now) {
  const stops = view?.stops ?? [];
  if (!stops.length) {
    return [];
  }
  const dark = state.theme === "dark";
  // Kept between frames, or deck.gl would rebuild every stop on each one.
  if (state.customersKey !== state.stopsKey) {
    state.customersKey = state.stopsKey;
    state.customers = stops.slice(1).map((position, i) => ({ position, stop: i + 1 }));
  }
  const unassigned = dark ? [190, 205, 235] : [70, 84, 112];
  let colorOf = null; // built only when deck.gl asks, as the plan changes
  const fillColor = (customer) => {
    colorOf ??= new Map(
      (routes ?? []).flatMap((route) => route.stops.map((stop) => [stop, route.color])),
    );
    return colorOf.get(customer.stop) ?? unassigned;
  };
  const beat = CALM ? 0.5 : (now % 2400) / 2400;
  return [
    new deck.ScatterplotLayer({
      id: "customers",
      data: state.customers,
      getPosition: (customer) => customer.position,
      getFillColor: fillColor,
      getLineColor: dark ? [5, 7, 14, 220] : [255, 255, 255, 235],
      getRadius: 3,
      radiusUnits: "pixels",
      stroked: true,
      lineWidthUnits: "pixels",
      getLineWidth: 1,
      pickable: true,
      updateTriggers: { getFillColor: `${key}-${state.theme}`, getLineColor: state.theme },
    }),
    new deck.ScatterplotLayer({
      id: "depot-ring",
      data: [stops[0]],
      getPosition: (position) => position,
      getRadius: 9 + 26 * beat,
      radiusUnits: "pixels",
      filled: false,
      stroked: true,
      lineWidthUnits: "pixels",
      getLineWidth: 1.6,
      getLineColor: dark ? [255, 255, 255] : [17, 26, 46],
      opacity: 0.8 * (1 - beat),
      updateTriggers: { getRadius: beat },
    }),
    new deck.ScatterplotLayer({
      id: "depot",
      data: [stops[0]],
      getPosition: (position) => position,
      getRadius: 7,
      radiusUnits: "pixels",
      getFillColor: dark ? [255, 255, 255] : [17, 26, 46],
      stroked: true,
      lineWidthUnits: "pixels",
      getLineWidth: 3,
      getLineColor: dark ? [255, 43, 214] : [194, 24, 127], // `--accent-2`
      updateTriggers: { getLineColor: state.theme, getFillColor: state.theme },
    }),
  ];
}

function tooltip({ object, layer }) {
  if (!object) {
    return null;
  }
  if (layer.id === "customers") {
    return { text: `Customer ${object.stop}`, className: "deck-tooltip" };
  }
  const cost = object.cost === null ? "" : ` · ${formatCost(object.cost, state.view?.edgeCost)}`;
  const count = object.stops.length;
  return {
    text: `Vehicle ${object.index + 1} · ${count} stop${count === 1 ? "" : "s"}${cost}`,
    className: "deck-tooltip",
  };
}

// ---- Start-up ----------------------------------------------------------------

const initialTheme = matchMedia("(prefers-color-scheme: light)").matches ? "light" : "dark";
state.theme = initialTheme;
document.documentElement.dataset.theme = initialTheme;

const map = new maplibregl.Map({
  container: "map",
  style: BASEMAPS[initialTheme],
  center: [21.0122, 52.2297],
  zoom: 10.6,
  attributionControl: { compact: true },
  dragRotate: false,
  pitchWithRotate: false,
});
map.touchZoomRotate.disableRotation();
map.addControl(new maplibregl.NavigationControl({ showCompass: false }), "bottom-right");
map.on("error", (event) => {
  // Without tiles, the routes still draw on a plain background.
  if (!map.isStyleLoaded() && event.error?.status !== 404 && !map.getLayer("page")) {
    map.setStyle(plainBasemap(state.theme));
  }
});

const overlay = new deck.MapboxOverlay({ interleaved: false, layers: [], getTooltip: tooltip });
map.addControl(overlay);

function pulseCost() {
  const rest = { transform: "scale(1)", filter: "drop-shadow(0 0 0 transparent)" };
  const peak = { transform: "scale(1.08)", filter: "drop-shadow(0 0 14px var(--accent))", offset: 0.25 };
  $("cost").animate([rest, peak, rest], { duration: 900, easing: "ease-out" });
}

function animate(now) {
  const view = state.view;
  const frame = shownFrame();
  const live = state.replay === null;
  const index = live ? state.frames.length - 1 : state.replay;
  overlay.setProps({
    layers: [
      ...oldPlanLayers(index, now),
      ...planLayers(frame, now),
      ...stopLayers(view, frame?.routes, `${frame?.id}`, now),
    ],
  });
  const target = headlineCost(view, frame);
  $("cost").classList.toggle("none", target === null);
  if (live && target !== null) {
    if (target < (state.bestCost ?? target) && !CALM) {
      pulseCost();
    }
    state.bestCost = Math.min(target, state.bestCost ?? target);
  }
  if (target !== null) {
    const shown = state.shownCost ?? target;
    // Eases over about 300 ms, whatever the frame rate.
    const ease = 1 - Math.exp(-(now - lastFrame) / 100);
    const next = CALM || Math.abs(shown - target) < 1 ? target : shown + (target - shown) * ease;
    state.shownCost = next;
    const text = formatCost(Math.round(next), view?.edgeCost);
    if ($("cost").textContent !== text) {
      $("cost").textContent = text;
    }
  } else if ($("cost").textContent !== "—") {
    $("cost").textContent = "—";
  }
  const clock = $("clock");
  if (clock && view?.running) {
    clock.textContent = formatClock(state.clockBase + (performance.now() - state.clockAt) / 1000);
  }
  lastFrame = now;
  requestAnimationFrame(animate);
}
let lastFrame = performance.now();
requestAnimationFrame(animate);

// ---- Controls ----------------------------------------------------------------

for (const button of document.querySelectorAll("#toggles [data-layer]")) {
  button.onclick = () => {
    const layer = button.dataset.layer;
    state.layers[layer] = !state.layers[layer];
    button.setAttribute("aria-pressed", String(state.layers[layer]));
  };
}
$("live").onclick = () => {
  state.replay = null;
  updatePanels();
  drawChart();
};
const chart = $("chart");
chart.onpointerdown = (event) => {
  chart.setPointerCapture(event.pointerId);
  scrub(event);
};
chart.onpointermove = (event) => {
  if (chart.hasPointerCapture(event.pointerId)) {
    scrub(event);
  }
};
window.addEventListener("resize", drawChart);

// ---- Streamlit's component protocol, without its npm package -----------------

function post(type, data = {}) {
  window.parent.postMessage({ isStreamlitMessage: true, type, ...data }, "*");
}

window.addEventListener("message", (event) => {
  if (event.data?.type === "streamlit:render") {
    render(event.data.args.view, event.data.theme);
  }
});
post("streamlit:componentReady", { apiVersion: 1 });
post("streamlit:setFrameHeight", { height: 820 });
