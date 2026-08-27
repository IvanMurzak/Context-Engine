/* ==========================================================================
   Context Editor — d1 Visual Direction Mockups — SHARED BEHAVIOR
   ==========================================================================
   Plain script (NOT a module — file:// blocks module/fetch CORS in some
   browsers, classic scripts don't). Wires the "Mockup Controls" overlay
   (theme / aurora variant / reduced-motion simulate / anatomy labels),
   the spring-pill segmented control, and a couple of small in-page demos
   (play bar, tree selection, tab switching, command palette) so the
   surfaces feel alive without any backend. MOCKUP-ONLY.
   ========================================================================== */
(function () {
  "use strict";

  var STORAGE = {
    theme: "ctx-mockup-theme",
    aurora: "ctx-mockup-aurora",
    motion: "ctx-mockup-motion",
    anatomy: "ctx-mockup-anatomy",
    flourish: "ctx-mockup-flourish",
  };

  function readStored(key, fallback) {
    try {
      var v = window.localStorage.getItem(key);
      return v === null ? fallback : v;
    } catch (e) {
      return fallback;
    }
  }
  function writeStored(key, value) {
    try { window.localStorage.setItem(key, value); } catch (e) { /* file:// storage may be partitioned/unavailable; degrade silently */ }
  }

  function applyTheme(theme) {
    document.documentElement.setAttribute("data-theme", theme);
    writeStored(STORAGE.theme, theme);
  }
  function applyAurora(variant) {
    document.documentElement.setAttribute("data-aurora-variant", variant);
    writeStored(STORAGE.aurora, variant);
  }
  function applyMotion(forced) {
    document.documentElement.classList.toggle("force-reduced-motion", forced);
    writeStored(STORAGE.motion, forced ? "reduced" : "normal");
  }
  function applyAnatomy(on) {
    document.documentElement.classList.toggle("show-anatomy", on);
    writeStored(STORAGE.anatomy, on ? "on" : "off");
  }
  /* d1 respin: the "Flourish" control (editor.html only — flourishes.html
     shows every treatment at once via hardcoded per-card data-flourish
     attributes and doesn't need this). Rewrites data-flourish on the ONE
     button carrying data-flourish-target, so the owner can A/B all ~6 new
     directions live in the real editor chrome, not just in isolation. */
  function applyFlourish(name) {
    var target = document.querySelector("[data-flourish-target]");
    if (target) target.setAttribute("data-flourish", name);
    writeStored(STORAGE.flourish, name);
  }
  function initFlourishSelect() {
    var select = document.querySelector("[data-flourish-select]");
    if (!select) return;
    select.addEventListener("change", function () { applyFlourish(select.value); });
  }

  /* ---- Generic spring-pill segmented control ----
     Markup contract:
       <div class="segmented" data-segmented data-role="theme">
         <span class="pill"></span>
         <button data-value="dark" class="active">Dark</button>
         <button data-value="light">Light</button>
       </div>
  */
  function positionPill(seg) {
    var pill = seg.querySelector(".pill");
    var active = seg.querySelector("button.active");
    if (!pill || !active) return;
    pill.style.width = active.offsetWidth + "px";
    pill.style.transform = "translateX(" + active.offsetLeft + "px)";
  }

  var ROLE_HANDLERS = {
    theme: applyTheme,
    aurora: applyAurora,
    motion: function (v) { applyMotion(v === "reduced"); },
    anatomy: function (v) { applyAnatomy(v === "on"); },
  };

  function initSegmented(seg) {
    var role = seg.getAttribute("data-role");
    var buttons = Array.prototype.slice.call(seg.querySelectorAll("button"));
    buttons.forEach(function (btn) {
      btn.addEventListener("click", function () {
        buttons.forEach(function (b) { b.classList.remove("active"); });
        btn.classList.add("active");
        positionPill(seg);
        var handler = ROLE_HANDLERS[role];
        if (handler) handler(btn.getAttribute("data-value"));
      });
    });
    positionPill(seg);
  }

  function restoreDefaults() {
    var theme = readStored(STORAGE.theme, "dark");
    var aurora = readStored(STORAGE.aurora, "a");
    var motion = readStored(STORAGE.motion, "normal");
    var anatomy = readStored(STORAGE.anatomy, "off");
    applyTheme(theme);
    applyAurora(aurora);
    applyMotion(motion === "reduced");
    applyAnatomy(anatomy === "on");

    // Flourish selector (editor.html only) — default matches the button's
    // own HTML attribute (ink-bloom) so a no-JS/first-paint render already
    // agrees with the control before this runs.
    var flourishTarget = document.querySelector("[data-flourish-target]");
    if (flourishTarget) {
      var flourish = readStored(STORAGE.flourish, flourishTarget.getAttribute("data-flourish") || "ink-bloom");
      applyFlourish(flourish);
      var flourishSelect = document.querySelector("[data-flourish-select]");
      if (flourishSelect) flourishSelect.value = flourish;
    }

    // Reflect restored state into any matching segmented controls' markup
    // so positionPill() lands the pill correctly on first paint.
    document.querySelectorAll("[data-segmented]").forEach(function (seg) {
      var role = seg.getAttribute("data-role");
      var current = { theme: theme, aurora: aurora, motion: motion === "reduced" ? "reduced" : "normal", anatomy: anatomy }[role];
      if (current === undefined) return;
      seg.querySelectorAll("button").forEach(function (b) {
        b.classList.toggle("active", b.getAttribute("data-value") === current);
      });
    });
  }

  /* ---- Small in-page demos (guarded — safe no-ops on pages without them) */

  function initClock() {
    var els = document.querySelectorAll("[data-live-clock]");
    if (!els.length) return;
    function tick() {
      var d = new Date();
      var s = [d.getHours(), d.getMinutes(), d.getSeconds()]
        .map(function (n) { return String(n).padStart(2, "0"); })
        .join(":");
      els.forEach(function (el) { el.textContent = s; });
    }
    tick();
    setInterval(tick, 1000);
  }

  function initTreeSelection() {
    document.querySelectorAll(".tree-row").forEach(function (row) {
      row.addEventListener("click", function () {
        var tree = row.closest(".tree");
        if (!tree) return;
        tree.querySelectorAll(".tree-row.selected").forEach(function (r) { r.classList.remove("selected"); });
        row.classList.add("selected");
        var label = row.querySelector(".name");
        document.querySelectorAll("[data-inspector-target]").forEach(function (el) {
          if (label) el.textContent = label.textContent;
        });
      });
    });
  }

  function initTabs() {
    document.querySelectorAll(".tabstrip").forEach(function (strip) {
      var tabs = Array.prototype.slice.call(strip.querySelectorAll(".tab[data-panel]"));
      if (!tabs.length) return;
      tabs.forEach(function (tab) {
        tab.addEventListener("click", function () {
          tabs.forEach(function (t) { t.classList.remove("active"); });
          tab.classList.add("active");
          var group = strip.closest(".dock-group");
          if (!group) return;
          group.querySelectorAll("[data-panel-body]").forEach(function (body) {
            body.style.display = body.getAttribute("data-panel-body") === tab.getAttribute("data-panel") ? "" : "none";
          });
        });
      });
    });
  }

  function initPlayBar() {
    var playBtn = document.querySelector("[data-play-btn]");
    var pauseBtn = document.querySelector("[data-pause-btn]");
    var stopBtn = document.querySelector("[data-stop-btn]");
    var dot = document.querySelector("[data-play-status-dot]");
    var label = document.querySelector("[data-play-status-label]");
    var timerEl = document.querySelector("[data-play-timer]");
    if (!playBtn || !dot || !label) return;

    var timer = null, elapsed = 0, state = "idle";

    function setState(next) {
      state = next;
      // d1 respin: mirror the real play-bar state onto the button itself
      // (harmless for every other treatment — only "state-linked" reads it)
      // so the "Pulse of Work" flourish can react to actual Play/Compile/Run
      // status instead of just decorating an idle button.
      playBtn.setAttribute("data-play-state", next);
      dot.className = "status-dot";
      label.className = "status-label";
      if (next === "compiling") {
        dot.classList.add("is-warn"); label.classList.add("is-warn"); label.textContent = "Compiling…";
      } else if (next === "running") {
        dot.classList.add("is-good"); label.classList.add("is-good"); label.textContent = "Running";
      } else if (next === "error") {
        dot.classList.add("is-bad"); label.classList.add("is-bad"); label.textContent = "Build failed";
      } else if (next === "paused") {
        dot.classList.add("is-idle"); label.classList.add("is-idle"); label.textContent = "Paused";
      } else {
        dot.classList.add("is-idle"); label.classList.add("is-idle"); label.textContent = "Ready";
      }
      if (pauseBtn) pauseBtn.disabled = next !== "running";
      if (stopBtn) stopBtn.disabled = next === "idle";
    }

    function tick() {
      elapsed += 1;
      var m = String(Math.floor(elapsed / 60)).padStart(2, "0");
      var s = String(elapsed % 60).padStart(2, "0");
      if (timerEl) timerEl.textContent = m + ":" + s;
    }

    playBtn.addEventListener("click", function () {
      if (state === "running") return;
      if (state === "paused") { setState("running"); timer = setInterval(tick, 1000); return; }
      setState("compiling");
      setTimeout(function () {
        setState("running");
        timer = setInterval(tick, 1000);
      }, 900);
    });
    if (pauseBtn) pauseBtn.addEventListener("click", function () {
      if (state !== "running") return;
      setState("paused");
      clearInterval(timer);
    });
    if (stopBtn) stopBtn.addEventListener("click", function () {
      setState("idle");
      clearInterval(timer);
      elapsed = 0;
      if (timerEl) timerEl.textContent = "00:00";
    });
    setState("idle");
  }

  function initPalette() {
    var trigger = document.querySelector("[data-palette-trigger]");
    var overlay = document.querySelector("[data-palette-overlay]");
    if (!trigger || !overlay) return;
    function open() { overlay.style.display = "flex"; var input = overlay.querySelector("input"); if (input) { input.value = ""; input.focus(); } }
    function close() { overlay.style.display = "none"; }
    trigger.addEventListener("click", open);
    overlay.addEventListener("click", function (e) { if (e.target === overlay) close(); });
    document.addEventListener("keydown", function (e) {
      if ((e.metaKey || e.ctrlKey) && e.key.toLowerCase() === "k") { e.preventDefault(); open(); }
      if (e.key === "Escape") close();
    });
  }

  function initGizmoHover() {
    document.querySelectorAll(".gizmo-arm").forEach(function (arm) {
      arm.addEventListener("mouseenter", function () { arm.classList.add("hover"); });
      arm.addEventListener("mouseleave", function () { arm.classList.remove("hover"); });
      arm.addEventListener("mousedown", function () { arm.classList.add("active"); });
      window.addEventListener("mouseup", function () { arm.classList.remove("active"); });
    });
  }

  document.addEventListener("DOMContentLoaded", function () {
    restoreDefaults();
    document.querySelectorAll("[data-segmented]").forEach(initSegmented);
    window.addEventListener("resize", function () {
      document.querySelectorAll("[data-segmented]").forEach(positionPill);
    });
    initClock();
    initTreeSelection();
    initTabs();
    initPlayBar();
    initPalette();
    initGizmoHover();
    initFlourishSelect();

    // Gate the theme cross-fade so first paint never animates in, then
    // arm it for every toggle after.
    requestAnimationFrame(function () {
      requestAnimationFrame(function () {
        document.body.classList.add("ctx-ready");
      });
    });
  });
})();
