// M9 spike s1 — Dockview-in-CEF probe matrix (THROWAWAY). External script only (strict CSP,
// script-src 'self'). Drives the vendored dockview-core@7.0.2 through the 6 design probes and
// emits ONE verdict three ways: window.__PROBE_RESULT__ (CEF host / poller), a console sentinel
// line "__PROBE_RESULT__ <json>" (CEF OnConsoleMessage), and POST /done?exit=<code> (the python
// headless-Chromium driver — the same POST-back channel as tools/web_golden_run.py).
'use strict';
(function () {
  var dv = window['dockview-core'];
  var results = { dockviewVersion: '7.0.2', probes: {}, cspViolations: [], meta: {} };

  // Capture CSP violations from the very first tick — probe 1 keys off script-src violations.
  document.addEventListener('securitypolicyviolation', function (e) {
    results.cspViolations.push({
      directive: e.effectiveDirective || e.violatedDirective,
      blockedURI: e.blockedURI,
      sample: (e.sample || '').slice(0, 80)
    });
  });

  function record(n, name, pass, notes, extra) {
    results.probes['p' + n] = Object.assign({ n: n, name: name, pass: pass, notes: notes }, extra || {});
  }
  function scriptCspViolated() {
    return results.cspViolations.some(function (v) { return /script-src/.test(v.directive || ''); });
  }
  function sleep(ms) { return new Promise(function (r) { setTimeout(r, ms); }); }
  function raf() { return new Promise(function (r) { requestAnimationFrame(function () { r(); }); }); }

  // --- component factory: plain panels + sandboxed-iframe (third-party) panels ----------------
  function createComponent(options) {
    if (options.name === 'iframe') {
      var iframe = document.createElement('iframe');
      iframe.className = 'iframe-panel';
      // allow-scripts WITHOUT allow-same-origin => opaque origin (design 04 §5 / 08).
      iframe.setAttribute('sandbox', 'allow-scripts');
      iframe.setAttribute('tabindex', '0');
      return {
        element: iframe,
        init: function () { iframe.src = 'panel.html'; }
      };
    }
    var el = document.createElement('div');
    el.className = 'panel-body';
    var h = document.createElement('h3');
    var body = document.createElement('div');
    el.appendChild(h); el.appendChild(body);
    return {
      element: el,
      // textContent only — never innerHTML with authored strings (design 04 §4 escaping backstop).
      init: function (params) { h.textContent = (params && params.title) || options.id; body.textContent = 'panel ' + options.id; }
    };
  }

  var host = document.getElementById('dock');
  var api = null;

  function countFloating(json) {
    if (!json) return 0;
    var f = json.floatingGroups;
    return Array.isArray(f) ? f.length : 0;
  }

  // === Probe 1 — docking core (splits / tabs / floating) under strict CSP + scheme ===========
  async function probe1() {
    try {
      api = dv.createDockview(host, { createComponent: createComponent, theme: dv.themeDark });
      var p1 = api.addPanel({ id: 'p1', component: 'default', title: 'Panel 1' });
      api.addPanel({ id: 'p2', component: 'default', title: 'Panel 2', position: { referencePanel: 'p1', direction: 'right' } });   // split
      api.addPanel({ id: 'p3', component: 'default', title: 'Panel 3', position: { referencePanel: 'p2', direction: 'within' } });  // tab
      api.addPanel({ id: 'pf', component: 'default', title: 'Floating', floating: true });                                           // floating group
      await raf(); await raf();
      var json = api.toJSON();
      var floating = countFloating(json);
      var okStructure = api.panels.length === 4 && api.groups.length >= 2 && floating >= 1;
      var cspOk = !scriptCspViolated();
      record(1, 'docking-core-under-csp', okStructure && cspOk,
        'panels=' + api.panels.length + ' groups=' + api.groups.length + ' floatingGroups=' + floating +
        '; scriptCspViolations=' + (cspOk ? 0 : 'YES') + '; totalCspViolations=' + results.cspViolations.length,
        { panels: api.panels.length, groups: api.groups.length, floatingGroups: floating });
    } catch (e) {
      record(1, 'docking-core-under-csp', false, 'threw: ' + (e && e.message));
    }
  }

  // === Probe 2 — sandboxed-iframe panel content (opaque origin, port bridge) =================
  async function probe2() {
    try {
      api.addPanel({ id: 'ext1', component: 'iframe', title: 'Ext Panel' });
      await raf();
      var iframe = host.querySelector('iframe.iframe-panel');
      if (!iframe) { record(2, 'sandboxed-iframe-panel', false, 'iframe element not mounted in panel'); return; }
      // wait for iframe document load
      await new Promise(function (res) {
        var to = setTimeout(res, 4000);
        iframe.addEventListener('load', function () { clearTimeout(to); res(); });
      });
      var rect = iframe.getBoundingClientRect();
      var laidOut = rect.width > 0 && rect.height > 0;
      // Bridge via MessageChannel port — opaque-origin frames can't be authed by origin string.
      var channel = new MessageChannel();
      var firstMsg = new Promise(function (res) {
        var to = setTimeout(function () { res(null); }, 4000);
        channel.port1.onmessage = function (ev) { clearTimeout(to); res(ev.data); };
      });
      channel.port1.start();
      iframe.contentWindow.postMessage({ type: 'bridge-port' }, '*', [channel.port2]);
      var msg = await firstMsg;
      var opaque = !!msg && msg.origin === 'null';
      var isolated = !!msg && msg.parentDomBlocked === true;
      // resize the panel's group and confirm the iframe follows
      var w0 = iframe.getBoundingClientRect().width;
      try { api.getPanel('ext1').api.setSize({ width: Math.round(w0 > 200 ? w0 - 80 : w0 + 80) }); } catch (e) {}
      await raf(); await sleep(60); await raf();
      var w1 = iframe.getBoundingClientRect().width;
      var resized = Math.abs(w1 - w0) > 4;
      var pass = laidOut && !!msg && opaque && isolated;
      record(2, 'sandboxed-iframe-panel', pass,
        'laidOut=' + laidOut + ' portMsg=' + !!msg + ' opaqueOrigin(null)=' + opaque +
        ' parentDomBlocked=' + isolated + ' resizePropagated=' + resized + ' (w ' + Math.round(w0) + '->' + Math.round(w1) + ')',
        { opaqueOrigin: opaque, parentDomBlocked: isolated, resizePropagated: resized });
    } catch (e) {
      record(2, 'sandboxed-iframe-panel', false, 'threw: ' + (e && e.message));
    }
  }

  // === Probe 3 — toJSON() / fromJSON() serialize-restore fidelity (incl. floating) ===========
  async function probe3() {
    try {
      var before = api.toJSON();
      var bPanels = api.panels.length, bGroups = api.groups.length, bFloat = countFloating(before);
      api.clear();
      await raf();
      var cleared = api.panels.length === 0;
      api.fromJSON(before);
      await raf(); await raf();
      var after = api.toJSON();
      var aPanels = api.panels.length, aGroups = api.groups.length, aFloat = countFloating(after);
      var fidelity = cleared && aPanels === bPanels && aGroups === bGroups && aFloat === bFloat;
      // structural byte-compare of the re-serialized layout (ignoring volatile ids is out of scope
      // for a spike; panel/group/floating counts + a re-toJSON round is the fidelity signal).
      record(3, 'serialize-restore-fidelity', fidelity,
        'panels ' + bPanels + '->' + aPanels + ' groups ' + bGroups + '->' + aGroups +
        ' floating ' + bFloat + '->' + aFloat + ' clearedTo0=' + cleared,
        { restoredPanels: aPanels, restoredFloating: aFloat });
    } catch (e) {
      record(3, 'serialize-restore-fidelity', false, 'threw: ' + (e && e.message));
    }
  }

  // === Probe 4 — v7 rejects non-http(s) popout URLs (popout API deliberately unused) =========
  async function probe4() {
    var mechanism = '', rejected = false, popoutOpened = false;
    try {
      var group = api.groups[0];
      var badUrl = 'context-editor://app/popout.html'; // the design's custom scheme — not http(s)
      try {
        var r = api.addPopoutGroup(group, { popoutUrl: badUrl });
        if (r && typeof r.then === 'function') {
          await r.then(function (ok) { popoutOpened = ok === true; mechanism = 'promise-resolved:' + ok; },
                       function (err) { rejected = true; mechanism = 'promise-rejected: ' + (err && err.message || err); });
        } else { mechanism = 'sync-returned:' + r; }
      } catch (e) {
        rejected = true; mechanism = 'threw-sync: ' + (e && e.message);
      }
      try { popoutOpened = popoutOpened || (typeof api.getPopouts === 'function' && api.getPopouts().length > 0); } catch (e) {}
      var mentionsScheme = /same-origin http|invalid popout URL|http\(s\)|protocol/i.test(mechanism);
      var pass = !popoutOpened; // the security-relevant outcome: no popout for a non-http(s) URL
      record(4, 'popout-nonhttp-rejected', pass,
        'popoutOpened=' + popoutOpened + ' rejected=' + rejected + ' schemeGuardMsg=' + mentionsScheme +
        ' mechanism=[' + mechanism + ']',
        { rejected: rejected, popoutOpened: popoutOpened, schemeGuardMessage: mentionsScheme });
    } catch (e) {
      record(4, 'popout-nonhttp-rejected', false, 'threw: ' + (e && e.message) + ' mechanism=' + mechanism);
    }
  }

  // === Probe 5 — per-extension process isolation (IsolateSandboxedIframes) ====================
  // OS-level renderer-process isolation is NOT observable from page JS — it is measured by the
  // CEF host (renderer subprocess count for N distinct-origin sandboxed frames) and recorded as a
  // Chromium FEATURE DEFAULT, not a CEF contract. Here we capture the DOM-level necessary
  // condition (opaque-origin sandboxed frames are mutually + parent isolated) and flag the
  // OS-process proof as the CEF-host residual.
  async function probe5() {
    try {
      var f = document.createElement('iframe');
      f.setAttribute('sandbox', 'allow-scripts');
      f.style.cssText = 'position:absolute;width:10px;height:10px;left:-9999px';
      document.body.appendChild(f);
      await new Promise(function (res) { var to = setTimeout(res, 2000); f.addEventListener('load', function () { clearTimeout(to); res(); }); });
      var crossFrameBlocked = true;
      try { var _ = f.contentWindow.document.cookie; crossFrameBlocked = false; } catch (e) { crossFrameBlocked = true; }
      document.body.removeChild(f);
      record(5, 'process-isolation', 'residual',
        'DOM-level opaque-origin isolation observable (crossFrameBlocked=' + crossFrameBlocked + '); ' +
        'OS renderer-process-count proof is the CEF-host residual (IsolateSandboxedIframes is a ' +
        'Chromium feature default, not a CEF contract) -- see src/ host + FINDINGS.md',
        { domIsolation: crossFrameBlocked, osProcessProof: 'cef-host-residual' });
    } catch (e) {
      record(5, 'process-isolation', 'residual', 'DOM probe threw: ' + (e && e.message) + '; OS proof = CEF-host residual');
    }
  }

  // === Probe 6 — a11y scan of Dockview chrome (tabs, drop zones) — feeds e16 scope ===========
  async function probe6() {
    try {
      var tabs = host.querySelectorAll('.dv-tab');
      var roled = host.querySelectorAll('[role]');
      var tablists = host.querySelectorAll('[role="tablist"]');
      var tabRole = host.querySelectorAll('[role="tab"]');
      var labelled = host.querySelectorAll('[aria-label],[aria-labelledby]');
      var live = document.querySelectorAll('[aria-live]');
      var focusable = 0, named = 0;
      tabs.forEach(function (t) {
        if (t.tabIndex >= 0 || t.querySelector('[tabindex="0"]')) focusable++;
        if ((t.getAttribute('aria-label') || t.textContent || '').trim()) named++;
      });
      var roles = {};
      roled.forEach(function (el) { var r = el.getAttribute('role'); roles[r] = (roles[r] || 0) + 1; });
      // Roving-tabindex is the WAI-ARIA tablist pattern: exactly one tab per tablist is in the tab
      // order (tabIndex 0), siblings are tabIndex -1 and arrow-key reachable. That is the PREFERRED
      // (more accessible) pattern, so focusableTabs === tablists is a PASS, as is all-tabs-focusable.
      var rovingOk = tablists.length > 0 && focusable === tablists.length;
      var allFocusable = tabs.length > 0 && focusable === tabs.length;
      var pass = tabs.length > 0 && named === tabs.length && tabRole.length === tabs.length &&
        tablists.length > 0 && live.length > 0 && (rovingOk || allFocusable);
      record(6, 'a11y-chrome-scan', pass,
        'tabs=' + tabs.length + ' focusableTabs=' + focusable + ' (rovingTabindex=' + rovingOk +
        ') namedTabs=' + named + ' role=tablist:' + tablists.length + ' role=tab:' + tabRole.length +
        ' [role]total=' + roled.length + ' [aria-label*]=' + labelled.length + ' [aria-live]=' + live.length +
        ' roles=' + JSON.stringify(roles),
        { tabs: tabs.length, focusableTabs: focusable, rovingTabindex: rovingOk, namedTabs: named,
          tablists: tablists.length, liveRegions: live.length, roles: roles });
    } catch (e) {
      record(6, 'a11y-chrome-scan', false, 'threw: ' + (e && e.message));
    }
  }

  function computeExit() {
    // Required (measured-here) probes must be strictly true; probe 5 'residual' is acceptable.
    var required = ['p1', 'p2', 'p3', 'p4', 'p6'];
    var ok = required.every(function (k) { return results.probes[k] && results.probes[k].pass === true; });
    return ok ? 0 : 1;
  }

  function finish() {
    results.meta.ua = navigator.userAgent;
    results.meta.origin = window.origin;
    results.meta.protocol = location.protocol;
    var exit = computeExit();
    results.meta.exit = exit;
    window.__PROBE_RESULT__ = results;
    try { console.log('__PROBE_RESULT__ ' + JSON.stringify(results)); } catch (e) {}
    var banner = document.getElementById('banner');
    if (banner) { banner.setAttribute('data-probe-exit', String(exit)); banner.textContent = 'probes done — exit ' + exit; }
    try { fetch('/done?exit=' + exit, { method: 'POST', body: JSON.stringify(results) }); } catch (e) {}
  }

  async function run() {
    // Overall safety net: never hang the driver — deliver whatever we have by 20s.
    var safety = setTimeout(finish, 20000);
    if (!dv || typeof dv.createDockview !== 'function') {
      record(1, 'docking-core-under-csp', false, 'window["dockview-core"].createDockview missing — bundle failed to load (likely a script-src CSP block)');
      clearTimeout(safety); finish(); return;
    }
    await probe1();
    await probe2();
    await probe3();
    await probe4();
    await probe5();
    await probe6();
    clearTimeout(safety);
    finish();
  }

  if (document.readyState === 'complete' || document.readyState === 'interactive') { run(); }
  else { window.addEventListener('DOMContentLoaded', run); }
})();
