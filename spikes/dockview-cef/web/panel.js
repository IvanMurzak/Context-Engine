// Sandboxed panel script (probe 2, THROWAWAY). Opaque-origin frame: proves the design's bridge
// model — the parent hands us a MessageChannel port at creation; we authenticate by holding the
// port, never by an origin string (our origin is "null"). We report opaque-origin + parent-DOM
// isolation + live size back over the port so the host probe can score layout/resize/focus.
'use strict';
(function () {
  var out = document.getElementById('out');
  var port = null;

  function parentDomBlocked() {
    // Cross-origin (opaque) frames must NOT be able to read the parent document.
    try {
      // eslint-disable-next-line no-unused-expressions
      var _ = window.parent.document.title; // should throw SecurityError
      return false;
    } catch (e) {
      return true;
    }
  }

  function report(kind) {
    if (!port) return;
    port.postMessage({
      type: kind,
      origin: window.origin, // "null" for an opaque-origin sandboxed frame
      parentDomBlocked: parentDomBlocked(),
      size: { w: window.innerWidth, h: window.innerHeight },
      focused: document.hasFocus(),
      t: Date.now()
    });
  }

  window.addEventListener('message', function (e) {
    // The parent posts the bridge port once at creation. e.origin is unreliable for opaque
    // frames, so we accept the port purely by the message shape + the transferred port.
    if (e.data && e.data.type === 'bridge-port' && e.ports && e.ports[0]) {
      port = e.ports[0];
      out.textContent = 'sandboxed panel ready — origin=' + window.origin +
        ' parentDomBlocked=' + parentDomBlocked();
      report('panel-ready');
      window.addEventListener('resize', function () { report('panel-resize'); });
      window.addEventListener('focus', function () { report('panel-focus'); });
    }
  });
})();
