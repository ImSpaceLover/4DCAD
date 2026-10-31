// User library: custom primitives + saved composite objects, persisted in
// localStorage and importable / exportable as JSON files.

import type { PrimitiveDef } from '../core/primitives.ts';
import { validatePrimitive } from '../core/primitives.ts';
import type { LibraryObject, PrimitiveRegistry } from '../core/scene.ts';

const KEY = '4dcad.library.v1';

export interface LibraryBundle {
  format: '4dcad-library';
  version: 1;
  primitives: PrimitiveDef[];
  objects: LibraryObject[];
}

export interface PrimitiveFile {
  format: '4dcad-primitive';
  version: 1;
  primitive: PrimitiveDef;
}

export class LibraryStore {
  primitives: PrimitiveDef[] = [];
  objects: LibraryObject[] = [];
  private listeners: (() => void)[] = [];

  constructor(private reg: PrimitiveRegistry) {
    try {
      const raw = localStorage.getItem(KEY);
      if (raw) {
        const b = JSON.parse(raw) as LibraryBundle;
        this.primitives = b.primitives ?? [];
        this.objects = b.objects ?? [];
      }
    } catch { /* storage unavailable: start empty */ }
    this.syncRegistry();
  }

  onChange(fn: () => void) { this.listeners.push(fn); }

  private syncRegistry() {
    for (const p of this.primitives) this.reg.set(p);
    for (const o of this.objects) for (const p of o.primitives) this.reg.set(p);
  }

  private persist() {
    this.syncRegistry();
    try { localStorage.setItem(KEY, JSON.stringify(this.bundle())); } catch { /* ignore */ }
    this.listeners.forEach((f) => f());
  }

  bundle(): LibraryBundle {
    return { format: '4dcad-library', version: 1, primitives: this.primitives, objects: this.objects };
  }

  savePrimitive(p: PrimitiveDef) {
    const i = this.primitives.findIndex((q) => q.id === p.id);
    if (i >= 0) this.primitives[i] = p; else this.primitives.push(p);
    this.persist();
  }
  deletePrimitive(id: string) {
    this.primitives = this.primitives.filter((p) => p.id !== id);
    this.persist();
  }
  saveObject(o: LibraryObject) {
    const i = this.objects.findIndex((q) => q.id === o.id);
    if (i >= 0) this.objects[i] = o; else this.objects.push(o);
    this.persist();
  }
  deleteObject(id: string) {
    this.objects = this.objects.filter((o) => o.id !== id);
    this.persist();
  }

  /** Import any of the supported JSON formats. Returns a short description. */
  importText(text: string): string {
    const data = JSON.parse(text);
    const addPrim = (p: PrimitiveDef) => {
      const errs = validatePrimitive(p);
      if (errs.length) throw new Error(`Invalid primitive "${p.name}": ${errs.join('; ')}`);
      const i = this.primitives.findIndex((q) => q.id === p.id);
      p.builtin = false;
      if (i >= 0) this.primitives[i] = p; else this.primitives.push(p);
    };
    let msg = '';
    if (data.format === '4dcad-primitive') { addPrim(data.primitive); msg = `primitive "${data.primitive.name}"`; }
    else if (data.format === '4dcad-object') { this.objects = this.objects.filter((o) => o.id !== data.id).concat([data]); msg = `object "${data.name}"`; }
    else if (data.format === '4dcad-library') {
      (data.primitives ?? []).forEach(addPrim);
      for (const o of data.objects ?? []) this.objects = this.objects.filter((q) => q.id !== o.id).concat([o]);
      msg = `${data.primitives?.length ?? 0} primitives, ${data.objects?.length ?? 0} objects`;
    } else if (data.format === '4dcad-scene') {
      (data.primitives ?? []).forEach(addPrim);
      msg = `${data.primitives?.length ?? 0} primitives from scene`;
    } else throw new Error('Unknown file format');
    this.persist();
    return msg;
  }
}

export function primitiveFile(p: PrimitiveDef): PrimitiveFile {
  return { format: '4dcad-primitive', version: 1, primitive: { ...p, builtin: false } };
}
