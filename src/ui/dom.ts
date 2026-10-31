// Tiny DOM helpers (no framework).

type Attrs = Record<string, any>;
type Child = Node | string | number | null | undefined | false | Child[];

export function h<K extends keyof HTMLElementTagNameMap>(tag: K, attrs: Attrs = {}, ...children: Child[]): HTMLElementTagNameMap[K] {
  const el = document.createElement(tag);
  for (const [k, v] of Object.entries(attrs)) {
    if (v === undefined || v === null || v === false) continue;
    if (k.startsWith('on') && typeof v === 'function') el.addEventListener(k.slice(2).toLowerCase(), v);
    else if (k === 'class') el.className = v;
    else if (k === 'style' && typeof v === 'object') Object.assign(el.style, v);
    else if (k in el && typeof v !== 'string') (el as any)[k] = v;
    else el.setAttribute(k, v === true ? '' : String(v));
  }
  const add = (c: Child) => {
    if (c === null || c === undefined || c === false) return;
    if (Array.isArray(c)) c.forEach(add);
    else el.append(c instanceof Node ? c : document.createTextNode(String(c)));
  };
  children.forEach(add);
  return el;
}

/** Numeric input that supports drag-to-scrub on its label. */
export function numberField(label: string, value: number, onChange: (v: number, final: boolean) => void, opts: { step?: number; min?: number; max?: number; title?: string } = {}) {
  const step = opts.step ?? 0.1;
  const input = h('input', { type: 'number', value: fmt(value), step: String(step), title: opts.title });
  if (opts.min !== undefined) input.min = String(opts.min);
  if (opts.max !== undefined) input.max = String(opts.max);
  const clampV = (v: number) => Math.min(opts.max ?? Infinity, Math.max(opts.min ?? -Infinity, v));
  input.addEventListener('input', () => { const v = parseFloat(input.value); if (Number.isFinite(v)) onChange(clampV(v), false); });
  input.addEventListener('change', () => { const v = parseFloat(input.value); if (Number.isFinite(v)) onChange(clampV(v), true); });
  const lab = h('label', { class: 'scrub', title: 'Drag to change' }, label);
  lab.addEventListener('pointerdown', (e) => {
    e.preventDefault();
    const x0 = e.clientX;
    const v0 = parseFloat(input.value) || 0;
    lab.setPointerCapture(e.pointerId);
    const move = (ev: PointerEvent) => {
      const v = clampV(v0 + Math.round((ev.clientX - x0) / 4) * step * (ev.shiftKey ? 0.1 : 1));
      input.value = fmt(v);
      onChange(v, false);
    };
    const up = () => {
      lab.removeEventListener('pointermove', move);
      lab.removeEventListener('pointerup', up);
      onChange(parseFloat(input.value) || 0, true);
    };
    lab.addEventListener('pointermove', move);
    lab.addEventListener('pointerup', up);
  });
  return h('div', { class: 'field' }, lab, input);
}

export function select<T extends string>(options: { value: T; label: string }[], value: T, onChange: (v: T) => void, attrs: Attrs = {}) {
  const s = h('select', attrs, ...options.map((o) => h('option', { value: o.value, selected: o.value === value }, o.label)));
  s.addEventListener('change', () => onChange(s.value as T));
  return s;
}

export const fmt = (v: number) => {
  const r = Math.round(v * 1000) / 1000;
  return String(Object.is(r, -0) ? 0 : r);
};

export const rgbToHex = (c: number[]) => '#' + c.map((x) => Math.round(Math.min(1, Math.max(0, x)) * 255).toString(16).padStart(2, '0')).join('');
export const hexToRgb = (s: string): [number, number, number] => [1, 3, 5].map((i) => parseInt(s.slice(i, i + 2), 16) / 255) as [number, number, number];

export function downloadText(filename: string, text: string) {
  const a = h('a', { href: URL.createObjectURL(new Blob([text], { type: 'application/json' })), download: filename });
  document.body.append(a);
  a.click();
  setTimeout(() => { URL.revokeObjectURL(a.href); a.remove(); }, 1000);
}

export function openTextFile(accept = '.json,application/json'): Promise<{ name: string; text: string } | null> {
  return new Promise((resolve) => {
    const input = h('input', { type: 'file', accept, style: { display: 'none' } });
    input.addEventListener('change', async () => {
      const f = input.files?.[0];
      resolve(f ? { name: f.name, text: await f.text() } : null);
      input.remove();
    });
    document.body.append(input);
    input.click();
  });
}

export function toast(msg: string, kind: 'info' | 'error' = 'info') {
  const t = h('div', { class: `toast ${kind}` }, msg);
  document.body.append(t);
  setTimeout(() => t.classList.add('show'), 10);
  setTimeout(() => { t.classList.remove('show'); setTimeout(() => t.remove(), 300); }, kind === 'error' ? 5000 : 2500);
}
