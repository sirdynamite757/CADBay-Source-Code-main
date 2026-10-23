/*
 * mod-tools.js - shared logic for the moderation pages.
 *
 * Access is enforced by the server: the pages themselves are named with a '+'
 * prefix, which the webserver refuses to serve to anyone below moderator, and
 * every /api/mod/ call is re-checked against the session cookie. The checks
 * here exist so the page can explain itself and avoid pointless requests, not
 * because they are what keeps the data safe.
 */

    (function () {
        const MOD_ROLES = ['mod', 'moderator', 'admin', 'dev', 'developer'];

        function currentUser() {
            if (typeof window.getSessionData === 'function') {
                return window.getSessionData();
            }
            try {
                const raw = localStorage.getItem('cadbay_user_data');
                return raw ? JSON.parse(raw) : null;
            } catch (error) {
                return null;
            }
        }

        function roleOf(user) {
            if (!user) return '';
            /* Mirror the server: a "premium" field of dev/developer marks a dev
             * account, exactly as account_service_get_status() does. */
            const premiumStatus = String(user.premium == null ? '' : user.premium).trim().toLowerCase();
            if (premiumStatus === 'dev' || premiumStatus === 'developer') return 'dev';
            return String(
                user.status || user.account_status || user.accountStatus || ''
            ).trim().toLowerCase();
        }

        function isModerator(user) {
            return MOD_ROLES.includes(roleOf(user));
        }

        /* Hide the page body and explain, rather than showing an empty UI that
           looks broken. */
        function showAccessDenied(message) {
            const shell = document.getElementById('mod-shell');
            const denied = document.getElementById('mod-access-denied');
            if (shell) shell.hidden = true;
            if (denied) {
                const detail = denied.querySelector('[data-denied-reason]');
                if (detail && message) detail.textContent = message;
                denied.hidden = false;
            }
        }

        /* Mark the current page in the shared navigation. */
        function markActiveNav() {
            const here = window.location.pathname.replace(/\.html$/, '').toLowerCase();
            document.querySelectorAll('.mod-nav a').forEach(link => {
                const target = new URL(link.href, window.location.origin).pathname
                    .replace(/\.html$/, '').toLowerCase();
                if (target === here) link.setAttribute('aria-current', 'page');
            });
        }

        function showMessage(text, isError) {
            const box = document.getElementById('mod-message');
            if (!box) return;
            box.textContent = text;
            box.className = 'mod-message ' + (isError ? 'error' : 'success');
            clearTimeout(showMessage._timer);
            showMessage._timer = setTimeout(() => { box.className = 'mod-message'; }, 4000);
        }

        async function fetchItems(store, status) {
            const suffix = status ? `?status=${encodeURIComponent(status)}` : '';
            const response = await fetch(window.cadbayApiUrl(`/mod/${store}${suffix}`), {
                credentials: 'include',
                cache: 'no-store'
            });
            if (response.status === 404 || response.status === 401 || response.status === 403) {
                throw new Error('Your account does not have access to moderation data.');
            }
            if (!response.ok) {
                throw new Error(`Could not load ${store} (HTTP ${response.status}).`);
            }
            const data = await response.json();
            return Array.isArray(data.items) ? data.items : [];
        }

        async function postAction(store, id, action) {
            const response = await fetch(window.cadbayApiUrl(`/mod/${store}/${encodeURIComponent(id)}/${action}`), {
                method: 'POST',
                credentials: 'include'
            });
            if (!response.ok) {
                throw new Error(`Could not ${action} ${id} (HTTP ${response.status}).`);
            }
            return response.json();
        }

        function statusClass(status) {
            const safe = String(status || 'unknown').toLowerCase().replace(/[^a-z-]/g, '');
            return `mod-status is-${safe || 'unknown'}`;
        }

        /* Actions offered for each store, keyed by the record's current status. */
        const ACTIONS = {
            approvals: {
                /* Approvals act on RFPs, so the labels reflect that and the actions
                   are re-checked against the staged rule by the server. */
                pending: [['approve', 'Approve', 'is-primary'], ['reject', 'Deny', 'is-danger']]
            },
            reports: {
                open: [['resolve', 'Resolve', 'is-primary'], ['dismiss', 'Dismiss', 'is-danger']],
                resolved: [['dismiss', 'Dismiss', 'is-danger']],
                dismissed: [['resolve', 'Resolve', 'is-primary']]
            },
            tasks: {
                open: [['start', 'Start', 'is-primary']],
                'in-progress': [['complete', 'Mark complete', 'is-primary']],
                complete: [['start', 'Reopen', '']]
            }
        };

        function actionButtons(store, item) {
            const options = (ACTIONS[store] || {})[String(item.status || '').toLowerCase()] || [];
            if (!options.length) return '';
            return options.map(([action, label, className]) =>
                `<button type="button" class="mod-action ${className}" ` +
                `data-store="${store}" data-id="${escapeHtml(item.id)}" data-action="${action}">` +
                `${escapeHtml(label)}</button>`
            ).join('');
        }

        function escapeHtml(value) {
            return String(value == null ? '' : value).replace(/[&<>"']/g, character => ({
                '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#039;'
            }[character]));
        }

        /* Approval rows carry the staged sign-off state, so they get a richer
           renderer than the generic one. */
        function renderApprovals(items) {
            const body = document.getElementById('mod-rows');
            if (!body) return;
            if (!items.length) {
                body.innerHTML = `<tr><td colspan="5" class="mod-empty">No RFPs are awaiting approval.</td></tr>`;
                return;
            }
            body.innerHTML = items.map(item => {
                const stageLabel = item.stage === '2'
                    ? 'Awaiting second moderator'
                    : 'Awaiting first review';
                const signatures = [`1: ${item.approval_1 || '—'}`, `2: ${item.approval_2 || '—'}`].join(' · ');
                const price = item.price ? `$${escapeHtml(item.price)}` : '—';
                return `
                <tr>
                    <td><strong>${escapeHtml(item.title || item.id)}</strong>
                        <div class="mod-detail">${escapeHtml(item.detail || '')}</div>
                        <div class="mod-detail">${escapeHtml(item.id)} · Budget ${price}</div></td>
                    <td>${escapeHtml(item.owner || '—')}</td>
                    <td><span class="${statusClass(item.status)}">${escapeHtml(item.status || 'pending')}</span>
                        <div class="mod-detail">${escapeHtml(stageLabel)}<br>${escapeHtml(signatures)}</div></td>
                    <td>${escapeHtml(stageLabel)}</td>
                    <td><div class="mod-actions">${actionButtons('approvals', item)}</div></td>
                </tr>`;
            }).join('');
        }

        /* Render a store into a table body, with a column set per store. */
        function renderTable(store, items) {
            if (store === 'approvals') {
                renderApprovals(items);
                return;
            }
            const body = document.getElementById('mod-rows');
            if (!body) return;
            if (!items.length) {
                body.innerHTML = `<tr><td colspan="5" class="mod-empty">Nothing to show.</td></tr>`;
                return;
            }
            body.innerHTML = items.map(item => `
            <tr>
                <td><strong>${escapeHtml(item.title || item.id)}</strong>
                    <div class="mod-detail">${escapeHtml(item.detail || '')}</div></td>
                <td>${escapeHtml(item.owner || '—')}</td>
                <td><span class="${statusClass(item.status)}">${escapeHtml(item.status || 'unknown')}</span></td>
                <td>${escapeHtml(item.updated || '—')}</td>
                <td><div class="mod-actions">${actionButtons(store, item)}</div></td>
            </tr>
        `).join('');
        }

        function renderSummary(items) {
            const target = document.getElementById('mod-summary');
            if (!target) return;
            const counts = {};
            items.forEach(item => {
                const key = String(item.status || 'unknown').toLowerCase();
                counts[key] = (counts[key] || 0) + 1;
            });
            const cards = Object.keys(counts).sort().map(key =>
                `<div class="mod-card"><span class="mod-card-label">${escapeHtml(key)}</span>` +
                `<span class="mod-card-value">${counts[key]}</span></div>`
            ).join('');
            target.innerHTML = cards || '';
        }

        /* Wire a moderation page up. */
        async function initPage(options) {
            markActiveNav();

            const user = currentUser();
            if (!isModerator(user)) {
                showAccessDenied('You need a moderator or developer account to view this page.');
                return;
            }

            const store = options.store;
            const filter = document.getElementById('mod-filter');
            const search = document.getElementById('mod-search');
            const refresh = document.getElementById('mod-refresh');
            let allItems = [];

            const draw = () => {
                const term = (search && search.value || '').trim().toLowerCase();
                const visible = term
                    ? allItems.filter(item =>
                        [item.title, item.owner, item.id, item.detail]
                            .some(value => String(value || '').toLowerCase().includes(term)))
                    : allItems;
                renderTable(store, visible);
                renderSummary(allItems);
            };

            const load = async () => {
                try {
                    allItems = await fetchItems(store, filter && filter.value ? filter.value : '');
                    draw();
                } catch (error) {
                    showMessage(error.message, true);
                    renderTable(store, []);
                }
            };

            if (filter) filter.addEventListener('change', load);
            if (refresh) refresh.addEventListener('click', load);
            if (search) search.addEventListener('input', draw);

            /* Delegated so the buttons still work after the table is redrawn. */
            const rows = document.getElementById('mod-rows');
            if (rows) {
                rows.addEventListener('click', async event => {
                    const button = event.target.closest('button[data-action]');
                    if (!button) return;
                    const { store: rowStore, id, action } = button.dataset;
                    button.disabled = true;
                    try {
                        const result = await postAction(rowStore, id, action);
                        /* Report the state the server actually recorded, which
                           depends on the staged approval rules (first vs second
                           moderator, or a lead approving outright). */
                        const status = result && result.status ? result.status : 'saved';
                        const suffix = result && result.stage === 1
                            ? ' awaiting a second moderator'
                            : '';
                        showMessage(`${id} \u2192 ${status}${suffix}.`, false);
                        await load();
                    } catch (error) {
                        showMessage(error.message, true);
                        button.disabled = false;
                    }
                });
            }

            await load();
        }

        window.modTools = { initPage, isModerator, currentUser, roleOf, showAccessDenied };
    })();
