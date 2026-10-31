// WebGL2 backend: the only file that touches the GPU API. A native port would
// replace this file (and keep core/ + shaders.ts as is): compile the two
// fragment shaders, upload ScenePrograms.data as an RGBA32F texture, draw a
// full-screen triangle.

import { buildFragmentShader, VERTEX_SHADER } from './shaders.ts';
import { TEX_W, type ScenePrograms } from '../core/program.ts';
import type { QualitySettings } from '../core/scene.ts';
import type { View } from '../core/camera.ts';
import type { Vec4 } from '../core/math4.ts';

export interface FrameParams {
  view: View;
  mode: 'full' | 'geo';
  grid: boolean;
  fovDeg: number;
  quality: QualitySettings;
  light: Vec4;
}

export class Renderer {
  readonly gl: WebGL2RenderingContext;
  private programs = new Map<'full' | 'geo', WebGLProgram>();
  private uniforms = new Map<WebGLProgram, Map<string, WebGLUniformLocation | null>>();
  private buf: WebGLBuffer;
  private tex: WebGLTexture;
  private scene: ScenePrograms | null = null;
  lastError: string | null = null;

  constructor(readonly canvas: HTMLCanvasElement) {
    const gl = canvas.getContext('webgl2', { antialias: false, preserveDrawingBuffer: true });
    if (!gl) throw new Error('WebGL2 is not available in this browser');
    this.gl = gl;
    this.buf = gl.createBuffer()!;
    gl.bindBuffer(gl.ARRAY_BUFFER, this.buf);
    gl.bufferData(gl.ARRAY_BUFFER, new Float32Array([-1, -1, 3, -1, -1, 3]), gl.STATIC_DRAW);
    this.tex = gl.createTexture()!;
    gl.bindTexture(gl.TEXTURE_2D, this.tex);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MIN_FILTER, gl.NEAREST);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MAG_FILTER, gl.NEAREST);
  }

  /** Uploads the scene data texture (cheap; done whenever the scene or selection changes). */
  setScene(p: ScenePrograms) {
    const gl = this.gl;
    this.scene = p;
    gl.bindTexture(gl.TEXTURE_2D, this.tex);
    gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGBA32F, TEX_W, p.height, 0, gl.RGBA, gl.FLOAT, p.data);
  }

  /** Compiles (once) and returns the program for a view mode. */
  program(mode: 'full' | 'geo'): WebGLProgram | null {
    const cached = this.programs.get(mode);
    if (cached) return cached;
    const gl = this.gl;
    const compile = (type: number, s: string) => {
      const sh = gl.createShader(type)!;
      gl.shaderSource(sh, s);
      gl.compileShader(sh);
      if (!gl.getShaderParameter(sh, gl.COMPILE_STATUS)) throw new Error(gl.getShaderInfoLog(sh) ?? 'shader compile error');
      return sh;
    };
    try {
      const prog = gl.createProgram()!;
      gl.attachShader(prog, compile(gl.VERTEX_SHADER, VERTEX_SHADER));
      gl.attachShader(prog, compile(gl.FRAGMENT_SHADER, buildFragmentShader(mode)));
      gl.bindAttribLocation(prog, 0, 'a_pos');
      gl.linkProgram(prog);
      if (!gl.getProgramParameter(prog, gl.LINK_STATUS)) throw new Error(gl.getProgramInfoLog(prog) ?? 'link error');
      this.programs.set(mode, prog);
      this.uniforms.set(prog, new Map());
      return prog;
    } catch (e) {
      this.lastError = String(e);
      console.error(this.lastError);
      return null;
    }
  }

  private u(prog: WebGLProgram, name: string) {
    const m = this.uniforms.get(prog)!;
    if (!m.has(name)) m.set(name, this.gl.getUniformLocation(prog, name));
    return m.get(name)!;
  }

  render(fp: FrameParams) {
    const gl = this.gl;
    const dpr = Math.min(window.devicePixelRatio || 1, 2) * fp.quality.resolutionScale;
    const w = Math.max(1, Math.floor(this.canvas.clientWidth * dpr));
    const h = Math.max(1, Math.floor(this.canvas.clientHeight * dpr));
    if (this.canvas.width !== w || this.canvas.height !== h) { this.canvas.width = w; this.canvas.height = h; }
    gl.viewport(0, 0, w, h);
    const sc = this.scene;
    const prog = this.program(fp.mode);
    if (!sc || !prog) return;
    gl.useProgram(prog);
    const U = (n: string) => this.u(prog, n);
    gl.activeTexture(gl.TEXTURE0);
    gl.bindTexture(gl.TEXTURE_2D, this.tex);
    gl.uniform1i(U('u_data'), 0);
    const main = fp.mode === 'geo' ? sc.geo : sc.full;
    gl.uniform2i(U('u_main'), main.start, main.len);
    const ws = [0, 0, 0, 0], wl = [0, 0, 0, 0];
    sc.worlds.slice(0, 4).forEach((r, i) => { ws[i] = r.start; wl[i] = r.len; });
    gl.uniform1iv(U('u_worldStart[0]'), ws);
    gl.uniform1iv(U('u_worldLen[0]'), wl);
    gl.uniform1i(U('u_nWorlds'), (fp.mode === 'geo' ? sc.hugGeo : sc.hugFull) ? Math.min(4, sc.worlds.length) : 0);
    gl.uniform1i(U('u_activeWorld'), sc.activeWorld);
    gl.uniform2f(U('u_res'), w, h);
    gl.uniform4fv(U('u_eye'), fp.view.eye);
    gl.uniformMatrix4fv(U('u_frame'), false, new Float32Array(fp.view.frame));
    gl.uniform1f(U('u_tanHalf'), Math.tan((fp.fovDeg * Math.PI) / 360));
    gl.uniform1i(U('u_maxSteps'), fp.quality.maxSteps);
    gl.uniform1i(U('u_shadows'), fp.quality.shadows ? 1 : 0);
    gl.uniform1i(U('u_ao'), fp.quality.ao ? 1 : 0);
    gl.uniform1i(U('u_grid'), fp.grid ? 1 : 0);
    gl.uniform1f(U('u_geoMaxStep'), fp.quality.geoMaxStep);
    gl.uniform1i(U('u_geoIters'), fp.quality.geoProjectIters);
    gl.uniform4fv(U('u_light'), fp.light);
    gl.bindBuffer(gl.ARRAY_BUFFER, this.buf);
    gl.enableVertexAttribArray(0);
    gl.vertexAttribPointer(0, 2, gl.FLOAT, false, 0, 0);
    gl.drawArrays(gl.TRIANGLES, 0, 3);
  }
}
