// 2D overlay: 4D axis gizmo, crosshair, mode / transform hints.

import { col, dot, type Vec4 } from '../core/math4.ts';
import type { App } from './app.ts';

const AXIS_COLORS = ['#e05555', '#5fcf5f', '#5588ee', '#c060e0'];

export class Hud {
  private ctx: CanvasRenderingContext2D;
  constructor(private canvas: HTMLCanvasElement, private app: App) {
    this.ctx = canvas.getContext('2d')!;
  }

  draw() {
    const { canvas, ctx, app } = this;
    const dpr = window.devicePixelRatio || 1;
    const w = canvas.clientWidth, hgt = canvas.clientHeight;
    if (canvas.width !== Math.floor(w * dpr) || canvas.height !== Math.floor(hgt * dpr)) {
      canvas.width = Math.floor(w * dpr);
      canvas.height = Math.floor(hgt * dpr);
    }
    ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
    ctx.clearRect(0, 0, w, hgt);
    const v = app.currentView();
    const right = col(v.frame, 0), up = col(v.frame, 1), fwd = col(v.frame, 2), ana = col(v.frame, 3);

    // axis gizmo: projection of world X/Y/Z/W onto the screen; the ring shows how much
    // of the axis points out of the visible 3D hyperplane (ana / surface normal).
    const cx = 60, cy = hgt - 60, R = 40;
    ctx.fillStyle = 'rgba(20,22,28,0.55)';
    ctx.beginPath(); ctx.arc(cx, cy, R + 14, 0, Math.PI * 2); ctx.fill();
    const axes = [0, 1, 2, 3].map((i) => {
      const e: Vec4 = [0, 0, 0, 0]; e[i] = 1;
      return { i, x: dot(e, right), y: dot(e, up), z: dot(e, fwd), out: Math.abs(dot(e, ana)) };
    }).sort((a, b) => b.z - a.z);
    for (const a of axes) {
      const ex = cx + a.x * R, ey = cy - a.y * R;
      ctx.strokeStyle = AXIS_COLORS[a.i];
      ctx.globalAlpha = 0.35 + 0.65 * (1 - a.out);
      ctx.lineWidth = 2;
      ctx.beginPath(); ctx.moveTo(cx, cy); ctx.lineTo(ex, ey); ctx.stroke();
      ctx.fillStyle = AXIS_COLORS[a.i];
      ctx.beginPath(); ctx.arc(ex, ey, 8, 0, Math.PI * 2); ctx.fill();
      if (a.out > 0.02) { ctx.strokeStyle = '#fff'; ctx.lineWidth = 1; ctx.beginPath(); ctx.arc(ex, ey, 8 + 4 * a.out, 0, Math.PI * 2 * a.out); ctx.stroke(); }
      ctx.fillStyle = '#111';
      ctx.font = 'bold 10px sans-serif';
      ctx.textAlign = 'center'; ctx.textBaseline = 'middle';
      ctx.fillText('XYZW'[a.i], ex, ey + 0.5);
    }
    ctx.globalAlpha = 1;

    // crosshair in explore mode
    if (app.mode === 'explore') {
      ctx.strokeStyle = 'rgba(255,255,255,0.8)';
      ctx.lineWidth = 1.5;
      ctx.beginPath();
      ctx.moveTo(w / 2 - 8, hgt / 2); ctx.lineTo(w / 2 + 8, hgt / 2);
      ctx.moveTo(w / 2, hgt / 2 - 8); ctx.lineTo(w / 2, hgt / 2 + 8);
      ctx.stroke();
    }

    // top-left status
    const lines = [
      `${app.mode === 'edit' ? 'EDIT' : 'EXPLORE'} · ${app.viewMode === 'full' ? 'Full 4D' : 'Geodesic 3D'} · ${app.fps.toFixed(0)} fps`,
      `eye (${v.eye.map((x) => x.toFixed(2)).join(', ')})`,
    ];
    if (app.mode === 'explore') {
      lines.push(document.pointerLockElement ? (app.viewMode === 'full'
        ? 'WASD move · Space/Shift up/down · Q/E move along W · Z/X turn into W · C/V turn sideways in W · R reset W · wheel speed · Esc release'
        : 'WASD move · Space/Shift up/down (tangent) · Q/E roll · wheel speed · Esc release')
        : 'Click the viewport to capture the mouse');
    } else {
      lines.push(app.transform ? app.transformHint()
        : 'MMB/RMB orbit · Shift+drag pan · Alt+drag rotate into W · wheel zoom · click select · G/R/S transform · Tab explore · V view mode');
    }
    ctx.font = '12px ui-monospace, monospace';
    ctx.textAlign = 'left'; ctx.textBaseline = 'top';
    lines.forEach((l, i) => {
      ctx.fillStyle = 'rgba(0,0,0,0.5)';
      const tw = ctx.measureText(l).width;
      ctx.fillRect(8, 8 + i * 18, tw + 10, 17);
      ctx.fillStyle = i === 2 && app.transform ? '#ffb347' : '#e8e8e8';
      ctx.fillText(l, 13, 10 + i * 18);
    });
  }
}
