#pragma once

// Injected into every main-frame document: element picker + DOM change tracking.
static const char* kInjectedScript = R"JS(
(() => {
  if (window.__curf) return;
  const C = window.__curf = { active: false, ov: null };
  // WebKit hosts use a message handler; Qt WebEngine hosts read tagged console messages.
  const dbg = console.debug.bind(console);
  const post = (m) => {
    try {
      if (window.webkit && webkit.messageHandlers && webkit.messageHandlers.curf) webkit.messageHandlers.curf.postMessage(m);
      else dbg('__curf__:' + JSON.stringify(m));
    } catch (e) {}
  };
  C.post = post;

  function sel(el) {
    if (!el || el.nodeType !== 1) return '';
    const parts = [];
    while (el && el.nodeType === 1 && el !== document.documentElement) {
      if (el.id) { parts.unshift('#' + CSS.escape(el.id)); break; }
      let s = el.tagName.toLowerCase();
      const par = el.parentElement;
      if (par) {
        const same = Array.from(par.children).filter(c => c.tagName === el.tagName);
        if (same.length > 1) s += ':nth-of-type(' + (same.indexOf(el) + 1) + ')';
      }
      parts.unshift(s);
      el = par;
    }
    return parts.join(' > ') || 'html';
  }
  C.sel = sel;

  C.describe = (el) => el ? ({
    selector: sel(el),
    tag: el.tagName.toLowerCase(),
    text: (el.innerText || el.textContent || '').trim().slice(0, 1000),
    html: el.outerHTML.slice(0, 4000),
    rect: (() => { const r = el.getBoundingClientRect(); return { x: r.x, y: r.y, w: r.width, h: r.height }; })()
  }) : null;

  function overlay() {
    if (!C.ov) {
      C.ov = document.createElement('div');
      C.ov.setAttribute('data-curf-overlay', '');
      C.ov.style.cssText = 'position:fixed;pointer-events:none;z-index:2147483647;border:2px solid #0a84ff;' +
        'background:rgba(10,132,255,.15);border-radius:3px;transition:all .05s;display:none';
      document.documentElement.appendChild(C.ov);
    }
    return C.ov;
  }

  C.setActive = (on) => {
    C.active = !!on;
    if (!C.active && C.ov) C.ov.style.display = 'none';
  };

  document.addEventListener('mouseover', (e) => {
    if (!C.active) return;
    const r = e.target.getBoundingClientRect(), o = overlay();
    Object.assign(o.style, { display: 'block', left: r.left + 'px', top: r.top + 'px', width: r.width + 'px', height: r.height + 'px' });
  }, true);

  const swallow = (e) => { if (C.active) { e.preventDefault(); e.stopPropagation(); } };
  ['mousedown', 'mouseup', 'pointerdown', 'pointerup'].forEach(t => document.addEventListener(t, swallow, true));
  document.addEventListener('click', (e) => {
    if (!C.active) return;
    e.preventDefault(); e.stopPropagation();
    C.setActive(false);
    post(Object.assign({ type: 'select', url: location.href }, C.describe(e.target)));
  }, true);
  document.addEventListener('keydown', (e) => {
    if (C.active && e.key === 'Escape') { C.setActive(false); post({ type: 'selectCancelled' }); }
  }, true);

  let queue = [], timer = null;
  const isOverlay = (n) => C.ov && (n === C.ov || C.ov.contains(n));
  new MutationObserver((ms) => {
    for (const m of ms) {
      if (isOverlay(m.target) || Array.from(m.addedNodes).some(isOverlay)) continue;
      if (queue.length >= 50) { queue.truncated = (queue.truncated || 0) + 1; continue; }
      const target = m.target.nodeType === 1 ? m.target : m.target.parentElement;
      const item = { kind: m.type, target: sel(target), added: m.addedNodes.length, removed: m.removedNodes.length };
      if (m.type === 'attributes') {
        item.attr = m.attributeName;
        item.old = m.oldValue;
        item.value = String(m.target.getAttribute(m.attributeName)).slice(0, 300);
      } else if (m.type === 'characterData') {
        item.old = (m.oldValue || '').slice(0, 300);
        item.value = String(m.target.data).slice(0, 300);
      } else if (m.addedNodes.length) {
        item.addedText = Array.from(m.addedNodes).map(n => (n.textContent || '').trim()).join(' ').slice(0, 300);
      }
      queue.push(item);
    }
    if (queue.length && !timer) {
      timer = setTimeout(() => {
        post({ type: 'mutations', url: location.href, items: queue, dropped: queue.truncated || 0 });
        queue = []; timer = null;
      }, 400);
    }
  }).observe(document.documentElement, { subtree: true, childList: true, attributes: true, characterData: true,
                                          attributeOldValue: true, characterDataOldValue: true });
})();
)JS";
