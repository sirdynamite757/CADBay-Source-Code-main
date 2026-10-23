/*
 * CADBay 3D engine — a small, dependency-free WebGL mesh viewer plus parsers
 * for STL (binary + ASCII) and OBJ. Used by viewers/3d.html.
 *
 * STEP files cannot be tessellated without a CAD kernel, so viewer.js exposes
 * parseSTEP() separately: it extracts header/metadata for display and reports
 * that a mesh preview is unavailable, rather than pretending to render it.
 */
(function (global) {
    'use strict';

    /* ------------------------------------------------------------------ */
    /* Parsers                                                            */
    /* ------------------------------------------------------------------ */

    /** Parse an ASCII or binary STL into {positions:Float32Array, count}. */
    function parseSTL(buffer) {
        var view = new DataView(buffer);

        // Binary STL: 80-byte header, uint32 triangle count, then 50 bytes/tri.
        var isBinary = true;
        if (buffer.byteLength >= 84) {
            var triCount = view.getUint32(80, true);
            var expected = 84 + triCount * 50;
            if (expected !== buffer.byteLength) isBinary = false;
        } else {
            isBinary = false;
        }

        // A leading "solid" can still be binary; trust the size check above.
        if (isBinary) {
            var n = view.getUint32(80, true);
            var positions = new Float32Array(n * 9);
            var offset = 84;
            for (var i = 0; i < n; i++) {
                offset += 12;                       // skip normal
                for (var v = 0; v < 3; v++) {
                    positions[i * 9 + v * 3 + 0] = view.getFloat32(offset, true);
                    positions[i * 9 + v * 3 + 1] = view.getFloat32(offset + 4, true);
                    positions[i * 9 + v * 3 + 2] = view.getFloat32(offset + 8, true);
                    offset += 12;
                }
                offset += 2;                        // attribute byte count
            }
            return { positions: positions, count: n * 3 };
        }

        // ASCII STL
        var text = new TextDecoder('utf-8').decode(buffer);
        var re = /vertex\s+(-?[\d.eE+-]+)\s+(-?[\d.eE+-]+)\s+(-?[\d.eE+-]+)/g;
        var vals = [];
        var m;
        while ((m = re.exec(text)) !== null) {
            vals.push(parseFloat(m[1]), parseFloat(m[2]), parseFloat(m[3]));
        }
        return { positions: new Float32Array(vals), count: vals.length / 3 };
    }

    /** Parse a wavefront OBJ into {positions, count} (triangulating quads). */
    function parseOBJ(text) {
        var verts = [];
        var tris = [];
        var lines = text.split(/\r?\n/);

        for (var i = 0; i < lines.length; i++) {
            var line = lines[i].trim();
            if (!line || line[0] === '#') continue;

            if (line.indexOf('v ') === 0) {
                var p = line.split(/\s+/);
                verts.push(parseFloat(p[1]), parseFloat(p[2]), parseFloat(p[3]));
            } else if (line.indexOf('f ') === 0) {
                var parts = line.split(/\s+/).slice(1);
                var idx = [];
                for (var j = 0; j < parts.length; j++) {
                    var raw = parts[j].split('/')[0];
                    var vi = parseInt(raw, 10);
                    if (isNaN(vi)) continue;
                    // OBJ indices are 1-based; negatives are relative.
                    idx.push(vi > 0 ? vi - 1 : verts.length / 3 + vi);
                }
                // Fan-triangulate (handles triangles and n-gons).
                for (var k = 1; k + 1 < idx.length; k++) {
                    tris.push(idx[0], idx[k], idx[k + 1]);
                }
            }
        }

        var positions = new Float32Array(tris.length * 3);
        for (var t = 0; t < tris.length; t++) {
            var base = tris[t] * 3;
            positions[t * 3 + 0] = verts[base + 0] || 0;
            positions[t * 3 + 1] = verts[base + 1] || 0;
            positions[t * 3 + 2] = verts[base + 2] || 0;
        }
        return { positions: positions, count: tris.length };
    }

    /**
     * Extract what we can from a STEP file without a CAD kernel.
     * Returns { header, entities, count } — metadata only, no mesh.
     */
    function parseSTEP(text) {
        var header = {};
        var h = text.match(/HEADER;([\s\S]*?)ENDSEC;/);
        if (h) {
            var body = h[1];
            var file = body.match(/FILE_NAME\s*\(([\s\S]*?)\);/);
            if (file) header.file_name = file[1].replace(/\s+/g, ' ').trim().slice(0, 200);
            var schema = body.match(/FILE_SCHEMA\s*\(([\s\S]*?)\);/);
            if (schema) header.schema = schema[1].replace(/\s+/g, ' ').trim().slice(0, 120);
        }
        var entityCount = (text.match(/^#\d+\s*=/gm) || []).length;
        return { header: header, entities: entityCount };
    }

    /* ------------------------------------------------------------------ */
    /* WebGL renderer                                                     */
    /* ------------------------------------------------------------------ */

    var VS = [
        'attribute vec3 aPos;',
        'uniform mat4 uMVP;',
        'uniform mat3 uNormalMat;',
        'varying vec3 vNormal;',
        'void main(){',
        '  gl_Position = uMVP * vec4(aPos, 1.0);',
        '  vNormal = normalize(uNormalMat * aPos);',
        '}'
    ].join('\n');

    var FS = [
        'precision mediump float;',
        'varying vec3 vNormal;',
        'void main(){',
        '  vec3 L = normalize(vec3(0.45, 0.75, 0.5));',
        '  float d = max(dot(normalize(vNormal), L), 0.0);',
        '  vec3 c = vec3(0.72, 0.16, 0.16) * (0.35 + 0.65 * d);',
        '  gl_FragColor = vec4(c, 1.0);',
        '}'
    ].join('\n');

    function compile(gl, type, src) {
        var s = gl.createShader(type);
        gl.shaderSource(s, src);
        gl.compileShader(s);
        if (!gl.getShaderParameter(s, gl.COMPILE_STATUS)) {
            throw new Error(gl.getShaderInfoLog(s) || 'shader compile failed');
        }
        return s;
    }

    function mat4Identity() {
        return new Float32Array([1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1]);
    }

    function mat4Perspective(fovy, aspect, near, far) {
        var f = 1 / Math.tan(fovy / 2);
        var nf = 1 / (near - far);
        return new Float32Array([
            f / aspect, 0, 0, 0,
            0, f, 0, 0,
            0, 0, (far + near) * nf, -1,
            0, 0, 2 * far * near * nf, 0
        ]);
    }

    function mat4Multiply(a, b) {
        var out = new Float32Array(16);
        for (var c = 0; c < 4; c++) {
            for (var r = 0; r < 4; r++) {
                out[c * 4 + r] =
                    a[0 * 4 + r] * b[c * 4 + 0] +
                    a[1 * 4 + r] * b[c * 4 + 1] +
                    a[2 * 4 + r] * b[c * 4 + 2] +
                    a[3 * 4 + r] * b[c * 4 + 3];
            }
        }
        return out;
    }

    function mat4RotX(a) {
        var c = Math.cos(a), s = Math.sin(a);
        return new Float32Array([1, 0, 0, 0, 0, c, s, 0, 0, -s, c, 0, 0, 0, 0, 1]);
    }
    function mat4RotY(a) {
        var c = Math.cos(a), s = Math.sin(a);
        return new Float32Array([c, 0, -s, 0, 0, 1, 0, 0, s, 0, c, 0, 0, 0, 0, 1]);
    }
    function mat4Translate(x, y, z) {
        return new Float32Array([1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, x, y, z, 1]);
    }
    function mat4Scale(s) {
        return new Float32Array([s, 0, 0, 0, 0, s, 0, 0, 0, 0, s, 0, 0, 0, 0, 1]);
    }

    /** Normalise a mesh to fit a unit box centred at the origin. */
    function centerAndScale(positions, count) {
        if (!count) return { positions: positions, count: 0, scale: 1 };
        var min = [Infinity, Infinity, Infinity];
        var max = [-Infinity, -Infinity, -Infinity];
        for (var i = 0; i < positions.length; i += 3) {
            for (var a = 0; a < 3; a++) {
                var v = positions[i + a];
                if (v < min[a]) min[a] = v;
                if (v > max[a]) max[a] = v;
            }
        }
        var cx = (min[0] + max[0]) / 2;
        var cy = (min[1] + max[1]) / 2;
        var cz = (min[2] + max[2]) / 2;
        var ex = Math.max(max[0] - min[0], 1e-6);
        var ey = Math.max(max[1] - min[1], 1e-6);
        var ez = Math.max(max[2] - min[2], 1e-6);
        var s = 2 / Math.max(ex, ey, ez);

        var out = new Float32Array(positions.length);
        for (var j = 0; j < positions.length; j += 3) {
            out[j + 0] = (positions[j + 0] - cx) * s;
            out[j + 1] = (positions[j + 1] - cy) * s;
            out[j + 2] = (positions[j + 2] - cz) * s;
        }
        return { positions: out, count: count, scale: s };
    }

    /**
     * Create a viewer bound to a canvas.
     * Returns { setMesh, start, stop, stats }.
     */
    function createViewer(canvas) {
        var gl = canvas.getContext('webgl') || canvas.getContext('experimental-webgl');
        if (!gl) throw new Error('WebGL is not available in this browser');

        var prog = gl.createProgram();
        gl.attachShader(prog, compile(gl, gl.VERTEX_SHADER, VS));
        gl.attachShader(prog, compile(gl, gl.FRAGMENT_SHADER, FS));
        gl.linkProgram(prog);
        if (!gl.getProgramParameter(prog, gl.LINK_STATUS)) {
            throw new Error(gl.getProgramInfoLog(prog) || 'program link failed');
        }
        gl.useProgram(prog);

        var aPos = gl.getAttribLocation(prog, 'aPos');
        var uMVP = gl.getUniformLocation(prog, 'uMVP');
        var uNormalMat = gl.getUniformLocation(prog, 'uNormalMat');

        var buffer = gl.createBuffer();
        var vertexCount = 0;
        var rotX = 0.5, rotY = 0.8;
        var dragging = false, lastX = 0, lastY = 0;
        var running = false;
        var zoom = 1;

        gl.enable(gl.DEPTH_TEST);
        gl.clearColor(0.06, 0.06, 0.07, 1.0);

        function setMesh(mesh) {
            var norm = centerAndScale(mesh.positions, mesh.count);
            gl.bindBuffer(gl.ARRAY_BUFFER, buffer);
            gl.bufferData(gl.ARRAY_BUFFER, norm.positions, gl.STATIC_DRAW);
            vertexCount = norm.count;
            gl.enableVertexAttribArray(aPos);
            gl.vertexAttribPointer(aPos, 3, gl.FLOAT, false, 0, 0);
        }

        function draw() {
            var dpr = window.devicePixelRatio || 1;
            var w = Math.max(1, Math.floor(canvas.clientWidth * dpr));
            var h = Math.max(1, Math.floor(canvas.clientHeight * dpr));
            if (canvas.width !== w || canvas.height !== h) {
                canvas.width = w;
                canvas.height = h;
            }
            gl.viewport(0, 0, w, h);
            gl.clear(gl.COLOR_BUFFER_BIT | gl.DEPTH_BUFFER_BIT);

            var proj = mat4Perspective(Math.PI / 4, w / h, 0.1, 100);
            var view = mat4Translate(0, 0, -3.2 / zoom);
            var rot = mat4Multiply(mat4RotY(rotY), mat4RotX(rotX));
            var mvp = mat4Multiply(proj, mat4Multiply(view, rot));

            gl.uniformMatrix4fv(uMVP, false, mvp);
            gl.uniformMatrix3fv(uNormalMat, false, new Float32Array([
                1, 0, 0, 0, 1, 0, 0, 0, 1
            ]));
            if (vertexCount) gl.drawArrays(gl.TRIANGLES, 0, vertexCount);
        }

        function frame() {
            if (!running) return;
            if (!dragging) rotY += 0.004;    // slow auto-spin when idle
            draw();
            requestAnimationFrame(frame);
        }

        canvas.addEventListener('mousedown', function (e) {
            dragging = true; lastX = e.clientX; lastY = e.clientY;
        });
        window.addEventListener('mouseup', function () { dragging = false; });
        window.addEventListener('mousemove', function (e) {
            if (!dragging) return;
            rotY += (e.clientX - lastX) * 0.01;
            rotX += (e.clientY - lastY) * 0.01;
            lastX = e.clientX; lastY = e.clientY;
        });
        canvas.addEventListener('wheel', function (e) {
            e.preventDefault();
            zoom *= (e.deltaY < 0) ? 1.1 : 0.9;
            zoom = Math.min(8, Math.max(0.25, zoom));
        }, { passive: false });

        return {
            setMesh: setMesh,
            start: function () { if (!running) { running = true; frame(); } },
            stop: function () { running = false; },
            stats: function () { return { vertices: vertexCount }; }
        };
    }

    global.CADBay3D = {
        parseSTL: parseSTL,
        parseOBJ: parseOBJ,
        parseSTEP: parseSTEP,
        createViewer: createViewer
    };
})(window);
