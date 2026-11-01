// viewer.js - three.js scene for STL meshes and CSV point clouds.
// Engineering convention: Z is "up" (camera.up = +Z), so no axis swapping is needed.
import * as THREE from 'three';
import { OrbitControls } from './vendor/OrbitControls.js';
import { colorAt } from './colormap.js';

export class Viewer {
  /** @param {HTMLElement} container  element that will hold the canvas */
  constructor(container) {
    this.container = container;
    this.onPick = null;            // callback(index | null)
    this.points = null;            // THREE.Points
    this.mesh = null;              // THREE.Mesh
    this.marker = null;            // highlight sphere
    this.pointData = null;         // {n, positions, diag}
    this._dirty = true;
    this.ok = false;

    try {
      this.renderer = new THREE.WebGLRenderer({ antialias: true, alpha: false });
    } catch (e) {
      container.textContent = 'WebGL is not available in this browser, so the 3-D viewer cannot start.';
      return;
    }
    this.ok = true;
    this.renderer.setPixelRatio(Math.min(window.devicePixelRatio || 1, 2));
    this.renderer.domElement.setAttribute('aria-label', '3-D viewer: drag to rotate, right-drag to pan, wheel to zoom');
    container.prepend(this.renderer.domElement);

    this.scene = new THREE.Scene();
    this.scene.background = new THREE.Color(0xf4f5f7);
    this.camera = new THREE.PerspectiveCamera(45, 1, 0.01, 1000);
    this.camera.up.set(0, 0, 1);
    this.camera.position.set(2, -3, 2);

    // lights: soft hemisphere + a directional light that follows the camera
    this.scene.add(new THREE.HemisphereLight(0xffffff, 0x9aa0aa, 2.0));
    this.headlight = new THREE.DirectionalLight(0xffffff, 2.2);
    this.scene.add(this.headlight, this.headlight.target);

    this.axes = new THREE.AxesHelper(1);
    this.scene.add(this.axes);

    this.controls = new OrbitControls(this.camera, this.renderer.domElement);
    this.controls.enableDamping = false;
    this.controls.addEventListener('change', () => this.requestRender());

    this.raycaster = new THREE.Raycaster();
    this._bindPicking();

    // resize handling (also fires when a hidden tab becomes visible)
    this._ro = new ResizeObserver(() => this.resize());
    this._ro.observe(container);
    this.resize();
    this._loop();
  }

  requestRender() { this._dirty = true; }

  _loop() {
    requestAnimationFrame(() => this._loop());
    if (!this._dirty) return;
    this._dirty = false;
    // headlight: from the camera towards the target
    this.headlight.position.copy(this.camera.position);
    this.headlight.target.position.copy(this.controls.target);
    this.renderer.render(this.scene, this.camera);
  }

  resize() {
    if (!this.ok) return;
    const w = this.container.clientWidth, h = this.container.clientHeight;
    if (w < 2 || h < 2) return;   // hidden tab
    this.renderer.setSize(w, h, false);
    this.renderer.domElement.style.width = w + 'px';
    this.renderer.domElement.style.height = h + 'px';
    this.camera.aspect = w / h;
    this.camera.updateProjectionMatrix();
    this.requestRender();
  }

  // ------------------------------------------------------------------ STL mesh
  /** positions: flat [x,y,z,...] (9 per triangle); normals: 3 per triangle (optional) */
  showMesh(positions, normals) {
    if (!this.ok) return;
    this.clearMesh();
    const pos = Float32Array.from(positions);
    const geo = new THREE.BufferGeometry();
    geo.setAttribute('position', new THREE.BufferAttribute(pos, 3));

    // use the file's facet normals when they are present and non-degenerate
    const nTri = pos.length / 9;
    let useGiven = normals && normals.length === nTri * 3;
    if (useGiven) {
      for (let i = 0; i < Math.min(nTri, 50) * 3; i += 3) {
        if (Math.hypot(normals[i], normals[i + 1], normals[i + 2]) < 1e-9) { useGiven = false; break; }
      }
    }
    if (useGiven) {
      const nrm = new Float32Array(nTri * 9);
      for (let t = 0; t < nTri; t++) for (let v = 0; v < 3; v++) {
        nrm.set([normals[3 * t], normals[3 * t + 1], normals[3 * t + 2]], t * 9 + v * 3);
      }
      geo.setAttribute('normal', new THREE.BufferAttribute(nrm, 3));
    } else {
      geo.computeVertexNormals();   // non-indexed -> flat face normals
    }
    geo.computeBoundingBox();
    geo.computeBoundingSphere();

    this.meshMaterial = new THREE.MeshStandardMaterial({
      color: 0x8fb0d6, roughness: 0.6, metalness: 0.1, side: THREE.DoubleSide,
      polygonOffset: true, polygonOffsetFactor: 1, polygonOffsetUnits: 1,
    });
    this.mesh = new THREE.Mesh(geo, this.meshMaterial);
    // thin outline along sharp edges (>25 deg) makes the shape readable; skipped for huge meshes
    this.edges = null;
    if (nTri <= 150000) {
      this.edges = new THREE.LineSegments(new THREE.EdgesGeometry(geo, 25), new THREE.LineBasicMaterial({ color: 0x1f2a37 }));
      this.mesh.add(this.edges);
    }
    this.scene.add(this.mesh);
    this.requestRender();
  }

  setWireframe(on) {
    if (this.meshMaterial) {
      this.meshMaterial.wireframe = on;
      if (this.edges) this.edges.visible = !on;   // the wireframe already shows every edge
      this.requestRender();
    }
  }

  clearMesh() {
    if (this.mesh) {
      this.scene.remove(this.mesh);
      this.mesh.geometry.dispose();
      this.meshMaterial.dispose();
      if (this.edges) { this.edges.geometry.dispose(); this.edges.material.dispose(); this.edges = null; }
      this.mesh = null;
    }
    this.requestRender();
  }

  // ------------------------------------------------------------ CSV point cloud
  /**
   * nodes: {x[],y[],z[],t[]}; colour by temperature using the named colormap.
   * bboxMin/bboxMax come from the server and scale the pick threshold.
   */
  showPoints(nodes, tmin, tmax, bboxMin, bboxMax, cmap) {
    if (!this.ok) return;
    this.clearPoints();
    const n = nodes.x.length;
    const pos = new Float32Array(n * 3);
    for (let i = 0; i < n; i++) { pos[3 * i] = nodes.x[i]; pos[3 * i + 1] = nodes.y[i]; pos[3 * i + 2] = nodes.z[i]; }
    const geo = new THREE.BufferGeometry();
    geo.setAttribute('position', new THREE.BufferAttribute(pos, 3));
    geo.setAttribute('color', new THREE.BufferAttribute(new Float32Array(n * 3), 3));
    geo.computeBoundingSphere();

    const diag = Math.hypot(bboxMax[0] - bboxMin[0], bboxMax[1] - bboxMin[1], bboxMax[2] - bboxMin[2]) || 1;
    this.pointData = { n, positions: pos, diag, t: nodes.t, tmin, tmax };
    const mat = new THREE.PointsMaterial({ size: 7, sizeAttenuation: false, vertexColors: true });
    this.points = new THREE.Points(geo, mat);
    this.scene.add(this.points);
    this.recolor(cmap);

    // pick tolerance: ~1.2 % of bbox diagonal (points are tiny, so be generous)
    this.raycaster.params.Points.threshold = diag * 0.012;
    // highlight marker (a sphere drawn on top of everything)
    if (!this.marker) {
      this.marker = new THREE.Mesh(
        new THREE.SphereGeometry(1, 20, 14),
        new THREE.MeshBasicMaterial({ color: 0xff2d95, depthTest: false, transparent: true, opacity: 0.9 }));
      this.marker.renderOrder = 10;
      this.marker.visible = false;
      this.scene.add(this.marker);
    }
    this.marker.scale.setScalar(diag * 0.012);
    this.marker.visible = false;
    this.requestRender();
  }

  /** Re-colour existing points (colormap change). */
  recolor(cmap) {
    if (!this.points) return;
    const { n, t, tmin, tmax } = this.pointData;
    const col = this.points.geometry.attributes.color;
    const span = tmax - tmin;
    for (let i = 0; i < n; i++) {
      const [r, g, b] = colorAt(cmap, span > 0 ? (t[i] - tmin) / span : 0.5);
      col.setXYZ(i, r, g, b);
    }
    col.needsUpdate = true;
    this.requestRender();
  }

  clearPoints() {
    if (this.points) {
      this.scene.remove(this.points);
      this.points.geometry.dispose();
      this.points.material.dispose();
      this.points = null;
      this.pointData = null;
    }
    if (this.marker) this.marker.visible = false;
    this.requestRender();
  }

  highlight(i) {
    if (!this.marker || !this.pointData) return;
    if (i === null) { this.marker.visible = false; } else {
      const p = this.pointData.positions;
      this.marker.position.set(p[3 * i], p[3 * i + 1], p[3 * i + 2]);
      this.marker.visible = true;
    }
    this.requestRender();
  }

  setPointsVisible(v) { if (this.points) { this.points.visible = v; if (this.marker && !v) this.marker.visible = false; this.requestRender(); } }
  setMeshVisible(v) { if (this.mesh) { this.mesh.visible = v; this.requestRender(); } }

  // ----------------------------------------------------------------- camera
  /** Bounding box of whatever is currently visible. */
  _bounds() {
    const box = new THREE.Box3();
    if (this.mesh && this.mesh.visible) box.union(this.mesh.geometry.boundingBox);
    if (this.points && this.points.visible) {
      this.points.geometry.computeBoundingBox();
      box.union(this.points.geometry.boundingBox);
    }
    return box;
  }

  /** Frame the visible bbox and size the axes helper to it. */
  fit() {
    if (!this.ok) return;
    const box = this._bounds();
    if (box.isEmpty()) return;
    const size = box.getSize(new THREE.Vector3());
    const centre = box.getCenter(new THREE.Vector3());
    const radius = Math.max(size.length() / 2, 1e-6);

    this.axes.scale.setScalar(Math.max(size.x, size.y, size.z, 1e-6) * 0.35);
    this.axes.position.copy(box.min);

    const dist = radius / Math.sin((this.camera.fov * Math.PI / 180) / 2) * 1.05;
    const dir = new THREE.Vector3(1, -1.3, 0.9).normalize();
    this.camera.position.copy(centre).addScaledVector(dir, dist);
    this.camera.near = dist / 1000;
    this.camera.far = dist * 100;
    this.camera.updateProjectionMatrix();
    this.controls.target.copy(centre);
    this.controls.update();
    this.requestRender();
  }

  // ---------------------------------------------------------------- picking
  _bindPicking() {
    const dom = this.renderer.domElement;
    let down = null;
    dom.addEventListener('pointerdown', (e) => { down = { x: e.clientX, y: e.clientY }; });
    dom.addEventListener('pointerup', (e) => {
      // a click is a press+release that barely moved (otherwise it was an orbit/pan)
      if (!down || Math.hypot(e.clientX - down.x, e.clientY - down.y) > 4) return;
      down = null;
      this._pick(e);
    });
  }

  _pick(e) {
    if (!this.points || !this.points.visible) return;
    const r = this.renderer.domElement.getBoundingClientRect();
    const ndc = new THREE.Vector2(((e.clientX - r.left) / r.width) * 2 - 1, -((e.clientY - r.top) / r.height) * 2 + 1);
    this.raycaster.setFromCamera(ndc, this.camera);
    const hits = this.raycaster.intersectObject(this.points, false);
    if (hits.length === 0) { if (this.onPick) this.onPick(null); return; }
    // several points can fall inside the threshold: take the one closest to the ray
    let best = hits[0];
    for (const h of hits) if (h.distanceToRay < best.distanceToRay) best = h;
    if (this.onPick) this.onPick(best.index);
  }

  /** Pixel position helper for tests: project node i to client coordinates. */
  projectNode(i) {
    const p = this.pointData.positions;
    const v = new THREE.Vector3(p[3 * i], p[3 * i + 1], p[3 * i + 2]).project(this.camera);
    const r = this.renderer.domElement.getBoundingClientRect();
    return { x: r.left + (v.x + 1) / 2 * r.width, y: r.top + (1 - v.y) / 2 * r.height };
  }
}
