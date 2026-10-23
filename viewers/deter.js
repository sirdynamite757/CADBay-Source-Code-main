/*
 * CADBay viewer deterrence layer (shared by all three viewers).
 *
 * IMPORTANT — READ THIS.
 * A web page CANNOT prevent screenshots, and cannot prevent a determined user
 * from saving content (the network tab / curl always works). What this file
 * does is raise the cost of casual copying:
 *
 *   - blocks right-click / save-as / drag-to-desktop
 *   - blocks copy / cut / paste of selections
 *   - blocks Ctrl+C, Ctrl+S, Ctrl+U, Ctrl+P, F12 and PrintScreen keydown
 *   - disables text selection and the OS context menu
 *   - overlays a per-viewer watermark carrying viewer, viewer id and time,
 *     so any screenshot or phone photo is traceable to who leaked it
 *
 * It is deterrence + attribution, NOT enforcement. Server-side conversion
 * (see viewers/README) is where real protection lives.
 */
(function () {
    'use strict';

    var WATERMARK_TEXT = '';

    function block(el, type, fn) { el.addEventListener(type, fn, true); }

    function install() {
        // --- context menu / dragging / saving ---
        block(document, 'contextmenu', function (e) { e.preventDefault(); return false; });
        block(document, 'dragstart', function (e) { e.preventDefault(); return false; });
        block(document, 'selectstart', function (e) {
            // allow selection inside explicitly-marked inputs
            if (e.target && e.target.closest && e.target.closest('input,textarea,[data-allow-select]')) return true;
            e.preventDefault();
            return false;
        });

        // --- copy / cut / paste ---
        ['copy', 'cut', 'paste'].forEach(function (ev) {
            block(document, ev, function (e) { e.preventDefault(); return false; });
        });

        // --- keyboard shortcuts ---
        block(window, 'keydown', function (e) {
            var k = (e.key || '').toLowerCase();

            // PrintScreen: clear the clipboard as a best-effort nudge
            if (e.key === 'PrintScreen') {
                try { navigator.clipboard.writeText(WATERMARK_TEXT); } catch (err) { /* ignore */ }
            }

            if (!e.ctrlKey && !e.metaKey) return true;

            // Block save / print / view-source / devtools / copy
            if (k === 's' || k === 'p' || k === 'u' || k === 'c' || k === 'x' ||
                k === 'a' || k === 'i' || k === 'j') {
                e.preventDefault();
                return false;
            }
            return true;
        });

        // --- watermark overlay ---
        var wm = document.createElement('div');
        wm.id = 'cadbay-watermark';
        wm.setAttribute('aria-hidden', 'true');
        wm.style.cssText = [
            'position:fixed', 'inset:0', 'pointer-events:none', 'z-index:2147483647',
            'display:flex', 'align-items:center', 'justify-content:center',
            'overflow:hidden', 'user-select:none'
        ].join(';');

        var inner = document.createElement('div');
        inner.style.cssText = [
            'transform:rotate(-30deg)', 'font:600 13px/2.4 monospace',
            'color:rgba(255,255,255,0.13)', 'text-shadow:0 0 1px rgba(0,0,0,0.25)',
            'white-space:pre', 'text-align:center'
        ].join(';');
        inner.textContent = repeatMark(WATERMARK_TEXT, 14);
        wm.appendChild(inner);
        document.body.appendChild(wm);

        // --- anti-print stylesheet ---
        var style = document.createElement('style');
        style.textContent =
            '@media print{body{display:none !important}' +
            'body::after{content:"Printing is disabled for this submission";' +
            'display:block;font:16px sans-serif;padding:2rem}}';
        document.head.appendChild(style);
    }

    function repeatMark(text, times) {
        var out = [];
        for (var i = 0; i < times; i++) out.push(text);
        return out.join('\n');
    }

    function setWatermark(text) { WATERMARK_TEXT = text || ''; }

    // Expose a small API for the individual viewers.
    window.CADBayDeter = {
        setWatermark: setWatermark,
        install: install,
        /*
         * Build the standard watermark string: submission + viewer + time.
         * The viewer id comes from ?viewer= or defaults to "anonymous".
         */
        makeMark: function (submissionId) {
            var params = new URLSearchParams(window.location.search);
            var viewer = params.get('viewer') || 'anonymous';
            var when = new Date().toISOString().replace('T', ' ').slice(0, 19);
            return 'CADBay ' + (submissionId || 'submission') +
                ' | viewer:' + viewer + ' | ' + when + ' | DO NOT DISTRIBUTE';
        }
    };

    if (document.readyState === 'loading') {
        document.addEventListener('DOMContentLoaded', install);
    } else {
        install();
    }
})();
