/* dc-lite.js: runs a design mock written as a Design Component (`<x-dc>` markup with {{holes}}, <sc-for>, <sc-if>,
 * and a `class Component extends DCLogic` script) straight from disk, no server and no claude.ai. Enough of the format
 * for the mocks in web/. Each setState builds the tree again and patches the page in place, so a slider being dragged
 * or a field being typed in keeps going.
 *
 *   <x-dc> ... </x-dc>
 *   <script type="text/x-dc" data-dc-script> class Component extends DCLogic { renderVals() { return {...}; } } </script>
 *   <script src="dc-lite.js"></script>   (last in <body>)
 */
(() => {
  class DCLogic {
    constructor(props) { this.props = props || {}; this.state = this.state || {}; }
    setState(o) { this.state = Object.assign({}, this.state, typeof o === 'function' ? o(this.state) : o); schedule(); }
    forceUpdate() { schedule(); }
  }
  window.DCLogic = DCLogic;

  const host = document.querySelector('x-dc'), script = document.querySelector('script[data-dc-script]');
  if (!host || !script) return;
  const helmet = host.querySelector('helmet'); // its fonts and styles go to <head>
  if (helmet) { for (const n of [...helmet.children]) document.head.appendChild(n); helmet.remove(); }
  const tpl = [...host.childNodes];
  for (const n of tpl) n.remove();
  const Component = new Function('DCLogic', script.textContent + '\nreturn Component;')(DCLogic);
  const comp = new Component({});

  // {{ path }}: a dotted lookup, or a literal
  const look = (path, scopes) => {
    path = path.trim();
    if (path === 'true') return true; if (path === 'false') return false; if (path === 'null') return null;
    if (/^-?\d+(\.\d+)?$/.test(path)) return +path;
    if (/^(['"]).*\1$/.test(path)) return path.slice(1, -1);
    const [head, ...rest] = path.split('.');
    let v; for (let i = scopes.length - 1; i >= 0; i--) if (scopes[i] && head in scopes[i]) { v = scopes[i][head]; break; }
    for (const k of rest) v = v == null ? undefined : v[k];
    return v;
  };
  const HOLE = /\{\{([^}]*)\}\}/g, WHOLE = /^\s*\{\{([^}]*)\}\}\s*$/;
  const text = (s, scopes) => s.replace(HOLE, (_, p) => { const v = look(p, scopes); return v == null || v === false ? '' : String(v); });
  const value = (s, scopes) => { const m = s.match(WHOLE); return m ? look(m[1], scopes) : text(s, scopes); };
  const EVENTS = { onclick: 'click', onchange: 'input', oninput: 'input', onkeydown: 'keydown', onkeyup: 'keyup', onblur: 'blur', onfocus: 'focus', onwheel: 'wheel', onmouseenter: 'mouseenter', onmouseleave: 'mouseleave', onpointerdown: 'pointerdown' };

  // a fresh tree from the template; handlers kept in el.__h, the field value in el.__v
  function build(node, scopes, out) {
    if (node.nodeType === 3) { out.appendChild(document.createTextNode(text(node.nodeValue, scopes))); return; }
    if (node.nodeType !== 1) return;
    const tag = node.localName;
    if (tag === 'sc-for') {
      const list = value(node.getAttribute('list') || '', scopes) || [], as = node.getAttribute('as') || 'item';
      list.forEach((item, i) => { const sc = scopes.concat([{ [as]: item, $index: i }]); for (const c of node.childNodes) build(c, sc, out); });
      return;
    }
    if (tag === 'sc-if') { if (value(node.getAttribute('value') || '', scopes)) for (const c of node.childNodes) build(c, scopes, out); return; }
    const el = node.namespaceURI && node.namespaceURI !== 'http://www.w3.org/1999/xhtml' ? document.createElementNS(node.namespaceURI, tag) : document.createElement(tag);
    el.__h = {};
    for (const a of node.attributes) {
      const name = a.name, low = name.toLowerCase();
      if (low.startsWith('hint-')) continue;
      const v = value(a.value, scopes);
      if (EVENTS[low]) { if (typeof v === 'function') el.__h[EVENTS[low]] = v; continue; }
      if (low === 'autofocus') { if (v) el.setAttribute('data-autofocus', ''); continue; }
      if (low === 'value') { el.__v = v == null ? '' : String(v); continue; }
      if (v === false || v == null) continue;
      el.setAttribute(name === 'className' ? 'class' : name, v === true ? (low.startsWith('aria-') ? 'true' : '') : String(v));
    }
    for (const c of node.childNodes) build(c, scopes, el);
    out.appendChild(el);
  }
  // one listener per event type, calling whatever handler the latest tree gave
  function wire(el) {
    if (el.nodeType !== 1) return;
    el.__l = el.__l || {};
    for (const t in el.__h) if (!el.__l[t]) { el.__l[t] = true; el.addEventListener(t, e => { const f = el.__h[t]; if (f) f(e); }, t === 'wheel' ? { passive: false } : undefined); }
    if (el.__v !== undefined && el.value !== el.__v) el.value = el.__v;
    for (const c of el.childNodes) wire(c);
  }
  function patch(parent, olds, news) {
    for (let i = 0; i < news.length; i++) {
      const o = olds[i], n = news[i];
      if (!o) { parent.appendChild(n); wire(n); continue; }
      if (o.nodeType !== n.nodeType || (o.nodeType === 1 && (o.localName !== n.localName || o.namespaceURI !== n.namespaceURI))) { parent.replaceChild(n, o); wire(n); continue; }
      if (o.nodeType === 3) { if (o.nodeValue !== n.nodeValue) o.nodeValue = n.nodeValue; continue; }
      for (const a of [...o.attributes]) if (!n.hasAttribute(a.name) && !(o.localName === 'canvas' && (a.name === 'width' || a.name === 'height'))) o.removeAttribute(a.name); // a canvas keeps the size its script gave it
      for (const a of n.attributes) if (o.getAttribute(a.name) !== a.value) o.setAttribute(a.name, a.value);
      o.__h = n.__h; o.__v = n.__v;
      wire(o);
      patch(o, [...o.childNodes], [...n.childNodes]);
    }
    for (let i = olds.length - 1; i >= news.length; i--) olds[i].remove();
  }

  let queued = false;
  function schedule() { if (!queued) { queued = true; queueMicrotask(() => { queued = false; draw(); }); } }
  function draw() {
    const frag = document.createElement('div'), vals = comp.renderVals();
    for (const n of tpl) build(n, [vals], frag);
    patch(host, [...host.childNodes], [...frag.childNodes]);
    const af = host.querySelector('[data-autofocus]');
    if (af && document.activeElement !== af) af.focus();
  }
  if (comp.componentDidMount) queueMicrotask(() => comp.componentDidMount());
  draw();
})();
