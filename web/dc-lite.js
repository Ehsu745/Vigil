/* dc-lite.js: runs a design mock written as a Design Component (`<x-dc>` markup with {{holes}}, <sc-for>, <sc-if>,
 * and a `class Component extends DCLogic` script) straight from disk, no server and no claude.ai. Enough of the format
 * for the mocks in web/: the whole tree is drawn again on every setState, keeping the focus where it was.
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
  // <helmet>: its fonts and styles go to <head>
  const helmet = host.querySelector('helmet');
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
  let autofocus = null;

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
    for (const a of node.attributes) {
      const name = a.name, low = name.toLowerCase();
      if (low.startsWith('hint-')) continue;
      const v = value(a.value, scopes);
      if (EVENTS[low]) { if (typeof v === 'function') el.addEventListener(EVENTS[low], v, low === 'onwheel' ? { passive: false } : undefined); continue; }
      if (low === 'autofocus') { if (v) autofocus = el; continue; }
      if (low === 'value' && 'value' in el) { el.value = v == null ? '' : v; continue; }
      if (v === false || v == null) continue;
      el.setAttribute(name === 'className' ? 'class' : name, v === true ? (low.startsWith('aria-') ? 'true' : '') : String(v));
    }
    for (const c of node.childNodes) build(c, scopes, tag === 'template' ? el.content : el);
    out.appendChild(el);
  }

  let queued = false;
  function schedule() { if (!queued) { queued = true; queueMicrotask(() => { queued = false; draw(); }); } }
  function draw() {
    // keep the focus (and the caret in a text field) on the element that had it, found again by its label
    const a = document.activeElement, key = a && a !== document.body ? a.getAttribute('aria-label') : null;
    const sel = a && 'selectionStart' in a ? [a.selectionStart, a.selectionEnd] : null;
    const vals = comp.renderVals(), frag = document.createDocumentFragment();
    autofocus = null;
    for (const n of tpl) build(n, [vals], frag);
    host.replaceChildren(frag);
    const again = autofocus || (key && host.querySelector('[aria-label="' + CSS.escape(key) + '"]'));
    if (again) { again.focus(); if (sel && !autofocus && 'setSelectionRange' in again) try { again.setSelectionRange(sel[0], sel[1]); } catch (e) { /* not a text field */ } }
  }
  if (comp.componentDidMount) queueMicrotask(() => comp.componentDidMount());
  draw();
})();
