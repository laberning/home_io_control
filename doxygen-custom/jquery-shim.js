// Minimal stand-in for the only three jQuery patterns doxygen-awesome-css still
// uses, in its darkmode-toggle / paragraph-link / fragment-copy-button scripts:
//
//   $(fn)                  -> run fn once the DOM is ready
//   $(document).ready(fn)  -> same
//   $(window).resize(fn)   -> add a window "resize" listener
//
// doxygen dropped its own jQuery dependency in 1.17; doxygen-awesome-css has not
// (still true at v2.4.2 and on main, checked 2026-09). This ~1 KB shim replaces
// the vendored 88 KB jQuery. Delete this file, and its <script> line in
// header.html + its HTML_EXTRA_FILES entry, once a doxygen-awesome release
// stops calling `$`.

(function () {
  "use strict";

  function ready(fn) {
    if (document.readyState === "loading") {
      document.addEventListener("DOMContentLoaded", fn);
    } else {
      // jQuery runs a late-registered ready callback asynchronously; match that.
      setTimeout(fn, 0);
    }
  }

  function wrap(target) {
    return {
      ready: function (fn) {
        ready(fn);
        return this;
      },
      resize: function (fn) {
        if (target === window) {
          window.addEventListener("resize", fn);
        }
        return this;
      },
    };
  }

  window.$ = function (arg) {
    if (typeof arg === "function") {
      ready(arg);
      return;
    }
    return wrap(arg);
  };
})();
