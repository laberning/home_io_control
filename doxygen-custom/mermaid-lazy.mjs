// Lazy Mermaid loader.
//
// doxygen (MERMAID_RENDER_MODE = CLIENT_SIDE) injects, into every generated page:
//
//     import mermaid from '<MERMAID_JS_URL>';
//     mermaid.initialize({ startOnLoad: true, theme: ... });
//
// MERMAID_JS_URL points here. This module's default export absorbs that
// `mermaid.initialize(...)` call as a no-op, and only pages that actually
// contain a `<pre class="mermaid">` go on to load the real ~3.5 MB
// mermaid.min.js (vendored next to this file). Diagram-free pages -- almost the
// whole site -- pay only this ~1 KB module.
//
// mermaid.min.js is the UMD build: a classic script that sets `globalThis.mermaid`
// as a side effect. It must be added with a <script> element -- loading it with
// import() would module-scope its top-level `var` and its own bootstrap line
// (`globalThis.mermaid = globalThis.__esbuild_esm_mermaid_nm.mermaid.default`)
// throws.
//
// doxygen-awesome's dark-mode toggle flips `html.dark-mode` at runtime and
// Mermaid bakes the theme into the SVG at render time, so each diagram is
// re-rendered from its source whenever that class changes.

const noop = () => {};
const stub = new Proxy({}, { get: () => noop });

function isDarkMode() {
  // doxygen-awesome always sets an explicit html.dark-mode / html.light-mode
  // class (on load and on every toggle), so this is authoritative.
  return document.documentElement.classList.contains("dark-mode");
}

function loadClassicScript(src) {
  return new Promise((resolve, reject) => {
    const el = document.createElement("script");
    el.src = src;
    el.onload = () => resolve();
    el.onerror = () => reject(new Error("could not load " + src));
    document.head.appendChild(el);
  });
}

async function boot() {
  const originals = Array.prototype.slice.call(document.querySelectorAll("pre.mermaid"));
  if (originals.length === 0) return;

  await loadClassicScript(new URL("mermaid.min.js", import.meta.url).href);
  const mermaid = globalThis.mermaid;
  if (!mermaid || typeof mermaid.render !== "function") return;

  // Replace each <pre class="mermaid"> with a persistent host <div> that keeps
  // the diagram source, so every (re-)render starts from a clean element.
  const hosts = originals.map(function (pre) {
    const host = document.createElement("div");
    host.className = "mermaid";
    host.dataset.mermaidSource = pre.textContent;
    pre.replaceWith(host);
    return host;
  });

  let seq = 0;
  let pending = null;

  async function renderAll() {
    mermaid.initialize({ startOnLoad: false, theme: isDarkMode() ? "dark" : "default" });
    seq += 1;
    for (let i = 0; i < hosts.length; i++) {
      try {
        const out = await mermaid.render("mermaid-" + seq + "-" + i, hosts[i].dataset.mermaidSource);
        hosts[i].innerHTML = out.svg;
        if (out.bindFunctions) out.bindFunctions(hosts[i]);
      } catch (err) {
        hosts[i].innerHTML =
          '<pre style="color:var(--warning-color,#a00)">mermaid render failed: ' +
          String(err && err.message ? err.message : err) +
          "</pre>";
      }
    }
  }

  function schedule() {
    clearTimeout(pending);
    pending = setTimeout(renderAll, 50); // one toggle flips two classes; coalesce
  }

  new MutationObserver(schedule).observe(document.documentElement, {
    attributes: true,
    attributeFilter: ["class"],
  });

  renderAll();
}

if (document.readyState === "loading") {
  document.addEventListener("DOMContentLoaded", boot);
} else {
  boot();
}

export default stub;
