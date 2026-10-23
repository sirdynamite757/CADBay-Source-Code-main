// keep the dynamic header, footer, token proscessing, and ads in a separate file so that it can be used on all pages without having to copy and paste the code into each page, this will also make it easier to update the header and footer in the future
if (typeof window.cadbayApiUrl !== 'function') {
    const configuredApiEndpoint = (() => {
        try {
            const value = JSON.parse(window.localStorage.getItem('cadbay_api_endpoint') || 'null');
            return value && /^[a-z0-9.-]+$/i.test(String(value.host || '')) &&
                /^\d+$/.test(String(value.port || '')) &&
                Number(value.port) >= 1 && Number(value.port) <= 65535 ? value : null;
        } catch (error) {
            return null;
        }
    })();
    window.cadbayApiUrl = function (path) {
        const suffix = String(path || '').replace(/^\/+/, '');
        const origin = configuredApiEndpoint
            ? `${window.location.protocol}//${configuredApiEndpoint.host}:${configuredApiEndpoint.port}`
            : window.location.origin;
        return `${origin}/api/${suffix}`;
    };
}

// document html tags here
// <div id="header"></div> <!-- dynamic header --> needs to always be at the top of the screen
// <div id="footer"></div> <!-- dynamic footer --> needs to be at rhe bottom of content
// <div id="auth-container"></div> <!-- dynamic login/register form -->
// <div id="ad-block"></div> <!-- dynamic ad panel --> these appear as square ads as of right now, but can be changed to any size or shape in the future, the ad panel will be hidden if the user is logged in and has premium, or if the ad is not found
// <div id="ad-sidebar"></div> <!-- dynamic ad panel --> these appear as vertical rectangle ads as of right now, but can be changed to any size or shape in the future, the ad panel will be hidden if the user is logged in and has premium, or if the ad is not found
// <div id="ad-banner"></div> <!-- dynamic ad panel --> these appear as horizontal banner ads as of right now, but can be changed to any size or shape in the future, the ad panel will be hidden if the user is logged in and has premium, or if the ad is not found






        function resolveFragmentUrl(path) {
            if (/^(https?:)?\/\//i.test(path)) {
                return path;
            }
            return new URL(path, window.location.href).href;
        }

        async function loadFragment(containerId, path) {
            const url = resolveFragmentUrl(path);
            const response = await fetch(url);
            if (!response.ok) {
                throw new Error(`Failed to load ${url}: ${response.status} ${response.statusText}`);
            }
            const html = await response.text();
            const container = document.getElementById(containerId);
            if (!container) {
                throw new Error(`Fragment container "${containerId}" not found`);
            }
            container.innerHTML = html;
            const scripts = Array.from(container.querySelectorAll('script'));
            await Promise.all(scripts.map(old => {
                const script = document.createElement('script');
                Array.from(old.attributes).forEach(attr => script.setAttribute(attr.name, attr.value));
                if (old.src) {
                    script.async = false;
                    script.src = old.src;
                    return new Promise((resolve, reject) => {
                        script.onload = resolve;
                        script.onerror = () => reject(new Error(`Failed to load script ${old.src}`));
                        old.parentNode.replaceChild(script, old);
                    });
                }
                script.textContent = old.textContent;
                old.parentNode.replaceChild(script, old);
                return Promise.resolve();
            }));
        }

        function ensureSiteIcon() {
            const iconHref = '/assets/CADBayIcon.png';
            let icon = document.head.querySelector('link[rel="icon"]');
            if (!icon) {
                icon = document.createElement('link');
                icon.rel = 'icon';
                document.head.appendChild(icon);
            }
            icon.type = 'image/png';
            icon.href = iconHref;
        }

        function setClientCookie(name, value, maxAge = 31536000) {
            const secure = window.location.protocol === 'https:' ? '; Secure' : '';
            document.cookie = `${encodeURIComponent(name)}=${encodeURIComponent(String(value))}; Path=/; Max-Age=${maxAge}; SameSite=Lax${secure}`;
        }

        function getClientCookie(name) {
            const prefix = `${encodeURIComponent(name)}=`;
            const cookie = document.cookie.split('; ').find(item => item.startsWith(prefix));
            return cookie ? decodeURIComponent(cookie.slice(prefix.length)) : null;
        }
        window.setClientCookie = setClientCookie;
        window.getClientCookie = getClientCookie;

        function initializeSharedFragments() {
            return loadFragment('header', 'header.html').then(() => {
            ensureSiteIcon();
            if (typeof initializeDarkMode === 'function') initializeDarkMode();
            if (typeof updateAccountMenu === 'function') updateAccountMenu();
            const session = typeof getSessionData === 'function' ? getSessionData() : null;
            if (session && typeof persistLoginSession === 'function') {
                // Repair older sessions that stored only premium="dev".
                persistLoginSession(session);
            }
            const accountSettingsSync = session ? fetch(cadbayApiUrl('/account-settings'), { credentials: 'include' })
                .then(response => response.ok ? response.json() : null)
                .then(account => {
                    if (!account || typeof persistLoginSession !== 'function') return;
                    const themeName = account.theme && String(account.theme).toLowerCase();
                    const accountStatus = normalizeAccountStatus(account) || session.status || session.account_status || '';
                    const mergedSession = {
                        ...session,
                        account_icon: account.account_icon || '',
                        theme: themeName || session.theme || null,
                        premium: account.premium ?? session.premium ?? '',
                        status: accountStatus,
                        account_status: accountStatus
                    };
                    persistLoginSession(mergedSession);
                    if (themeName && typeof applyThemePreference === 'function') {
                        applyThemePreference(themeName, true);
                    }

                    if (typeof updateAccountMenu === 'function') updateAccountMenu();
                })
                .catch(error => {
                    console.warn('Account status sync failed:', error);
                }) : Promise.resolve();
            const fragments = [
                loadFragment('footer', 'footer.html')
            ];
            if (document.getElementById('auth-container')) {
                fragments.push(loadFragment('auth-container', 'loginRegister.html'));
            }
            if (document.getElementById('sidebar')) {
                fragments.push(loadFragment('sidebar', 'sidebar.html'));
            }
            if (document.getElementById('ad-banner')) {
                fragments.push(loadAdInto('ad-banner', 'banner'));
            } 
            if (document.getElementById('ad-sidebar')) {
                fragments.push(loadAdInto('ad-sidebar', 'sidebar'));
            }
            if (document.getElementById('ad-block')) {
                fragments.push(loadAdInto('ad-block', 'block'));
            }
            return Promise.all([accountSettingsSync, ...fragments]);
            }).catch(error => {
                console.error('Failed to load fragments:', error);
            });
        }

        if (document.readyState === 'loading') {
            document.addEventListener('DOMContentLoaded', initializeSharedFragments, { once: true });
        } else {
            initializeSharedFragments();
        }

        function safeParseJson(text) {
            try {
                return JSON.parse(text);
            } catch (err) {
                console.warn('Failed to parse saved user data JSON:', err);
                return null;
            }
        }

        function updateAccountMenu() {
            const userDataJson = localStorage.getItem('cadbay_user_data');
            const accountDropdown = document.getElementById('account-dropdown');
            const loginBtn = document.getElementById('login-btn');
            const accountBtn = document.getElementById('account-btn');
            const devAdsToggle = document.getElementById('header-dev-ads-toggle');

            const userData = userDataJson ? safeParseJson(userDataJson) : null;
            const accountName = userData && (userData.username || userData.user_id);
            const loggedIn = Boolean(userData && userData.user_id);

            if (loggedIn) {
                if (accountDropdown) accountDropdown.style.display = 'inline-block';
                if (loginBtn) loginBtn.style.display = 'none';
                if (accountBtn) {
                    const accountLabel = accountBtn.querySelector('.account-label');
                    if (accountLabel) {
                        accountLabel.textContent = accountName;
                    } else {
                        accountBtn.textContent = `👤 ${accountName} ▼`;
                    }
                    const accountIcon = accountBtn.querySelector('#account-icon');
                    const iconPath = getClientCookie('cadbay_account_icon') || userData.account_icon || userData.icon || '/assets/accountIconsDefault/robot.png';
                    if (accountIcon) {
                        accountIcon.src = iconPath;
                        accountIcon.addEventListener('error', () => {
                            accountIcon.src = '/assets/accountIconsDefault/robot.png';
                        }, { once: true });
                        accountIcon.onerror = () => {
                            accountIcon.src = '/assets/accountIconsDefault/robot.png';
                        };
                    }
                    setClientCookie('cadbay_account_icon', iconPath);
                }
            } else {
                if (accountDropdown) accountDropdown.style.display = 'none';
                if (loginBtn) loginBtn.style.display = 'inline-block';
            }
            if (devAdsToggle) {
                const dev = typeof window.isDevAccount === 'function' && window.isDevAccount(userData);
                devAdsToggle.hidden = !dev;
                const checkbox = devAdsToggle.querySelector('input[type="checkbox"]');
                if (!dev) {
                    if (checkbox) {
                        checkbox.checked = false;
                        checkbox.disabled = true;
                    }
                }
                if (dev) {
                    const adsEnabled = getClientCookie('cadbay_dev_ads_enabled') !== 'false';
                    if (!checkbox) return;
                    checkbox.disabled = false;
                    checkbox.checked = adsEnabled;
                    devAdsToggle.classList.toggle('ads-disabled', !adsEnabled);
                    if (!devAdsToggle.dataset.bound) {
                        devAdsToggle.dataset.bound = 'true';
                        checkbox.addEventListener('change', () => {
                            const enabled = checkbox.checked;
                            if (typeof window.setDeveloperAdsEnabled === 'function') {
                                window.setDeveloperAdsEnabled(enabled);
                            } else if (typeof window.refreshAdVisibility === 'function') {
                                setClientCookie('cadbay_dev_ads_enabled', enabled);
                                window.refreshAdVisibility();
                            }
                            updateAccountMenu();
                        });
                    }
                }
            }
        }

        function isPremiumActive(userData) {
            if (!userData) return false;

            const status = String(userData.status || userData.account_status || userData.accountStatus || '').trim().toLowerCase();
            if (['premium', 'enterprise', 'insider'].includes(status)) {
                return true;
            }

            if (userData.premium === true || userData.premium === 'true' || userData.premium === 1 || userData.premium === '1') {
                return true;
            }

            if (userData.premium === false || userData.premium === 'false' || userData.premium === 0 || userData.premium === '0') {
                return false;
            }

            if (typeof userData.premium === 'string') {
                const normalized = userData.premium.trim();
                if (!normalized || normalized === 'none' || normalized === 'null') return false;
                const parts = normalized.split('-');
                if (parts.length === 3) {
                    const expiry = new Date(Number(parts[2]), Number(parts[0]) - 1, Number(parts[1]), 23, 59, 59);
                    return !Number.isNaN(expiry.getTime()) && expiry > new Date();
                }
                return normalized.toLowerCase() === 'premium';
            }

            return Boolean(userData.premium);
        }

        function normalizeAccountStatus(userData) {
            if (!userData) return '';

            const explicitStatus = String(
                userData.status || userData.account_status || userData.accountStatus || ''
            ).trim().toLowerCase();
            const premiumStatus = String(userData.premium || '').trim().toLowerCase();

            if (premiumStatus === 'dev' || premiumStatus === 'developer') {
                return 'dev';
            }
            return explicitStatus;
        }

        function isDevAccount(userData) {
            return ['dev', 'developer'].includes(normalizeAccountStatus(userData));
        }


        // set user status to returned value, the statsu is stored in the cadbay_user_data local storage item, and is also sent to the server when the user logs in or out, this is used to determine if the user is premium or not, and to show or hide ads accordingly
        function setUserStatus(status) {
            const session = getStoredSessionData();
            if (!session) return;
            session.status = String(status || '').trim();
            session.account_status = String(status || '').trim();
            persistLoginSession(session);
            updateAccountMenu();
        }




        function getStoredSessionData() {
            if (typeof getSessionData === 'function') return getSessionData();
            return safeParseJson(localStorage.getItem('cadbay_user_data'));
        }

        function areAdsEnabled(userData) {
            if (isPremiumActive(userData)) return false;
            if (isDevAccount(userData)) {
                return getClientCookie('cadbay_dev_ads_enabled') !== 'false';
            }
            return true;
        }

        function setAdsVisible(visible) {
            document.querySelectorAll('#ad-panel, #ad-block, #ad-sidebar, #ad-banner, #ad-content').forEach(element => {
                element.hidden = !visible;
                if (!visible) element.replaceChildren();
            });
        }

        function refreshAdVisibility() {
            const session = getStoredSessionData();
            const enabled = areAdsEnabled(session);
            setAdsVisible(enabled);
            return enabled;
        }

        function setDeveloperAdsEnabled(enabled) {
            const session = getStoredSessionData();
            if (!isDevAccount(session)) return;
            setClientCookie('cadbay_dev_ads_enabled', Boolean(enabled));
            if (!Boolean(enabled)) {
                setAdsVisible(false);
                if (typeof window.onAdsVisibilityChanged === 'function') {
                    window.onAdsVisibilityChanged(false);
                }
                return;
            }
            refreshAdVisibility();
            if (typeof window.loadAds === 'function') window.loadAds();
            if (typeof window.onAdsVisibilityChanged === 'function') {
                window.onAdsVisibilityChanged(true);
            }
        }

        window.isPremiumActive = isPremiumActive;
        window.isDevAccount = isDevAccount;
        window.areAdsEnabled = areAdsEnabled;
        window.refreshAdVisibility = refreshAdVisibility;
        window.setDeveloperAdsEnabled = setDeveloperAdsEnabled;
        window.loadAds = function() {
            const loads = [];
            if (document.getElementById('ad-banner')) loads.push(loadAdInto('ad-banner', 'banner'));
            if (document.getElementById('ad-sidebar')) loads.push(loadAdInto('ad-sidebar', 'sidebar'));
            if (document.getElementById('ad-block')) loads.push(loadAdInto('ad-block', 'block'));
            if (document.getElementById('ad-content')) loads.push(loadAd(null));
            return Promise.all(loads);
        };

        const monitorButton = document.getElementById('monitor-button');
        const userDataStr = localStorage.getItem('cadbay_user_data');
        const urlParams = new URLSearchParams(window.location.search);
        const requestedAd = urlParams.get('ad');

        const userData = userDataStr ? safeParseJson(userDataStr) : null;
        if (userDataStr && userData) {
            if (monitorButton) {
                monitorButton.style.display = 'inline-block';
            }
        } else {
            if (monitorButton) {
                monitorButton.style.display = 'none';
            }
        }
        if (refreshAdVisibility()) {
            loadAd(requestedAd);
        }

        function applyThemePreference(themeName, persistPreference = true) {
            const normalizedTheme = String(themeName || '').trim().toLowerCase();
            if (normalizedTheme === 'sith') {
                applySithMode(true, persistPreference);
                return;
            }

            const darkModeToggle = document.getElementById('darkModeToggle');
            const toggleIcon = document.getElementById('toggleIcon');
            const body = document.body;
            const isDarkMode = normalizedTheme === 'dark';

            if (body) {
                body.classList.toggle('dark-mode', isDarkMode);
                body.classList.remove('sith-mode');
            }

            if (toggleIcon) {
                toggleIcon.src = isDarkMode ? '/assets/CADBayDark.png' : '/assets/CADBayLight.png';
                toggleIcon.onerror = () => {
                    toggleIcon.onerror = null;
                    toggleIcon.src = isDarkMode ? '/assets/CADBayDark.webp' : '/assets/CADBayLight.webp';
                };
                toggleIcon.alt = isDarkMode ? 'Dark mode' : 'Light mode';
            }
            if (darkModeToggle) {
                darkModeToggle.setAttribute('aria-pressed', String(isDarkMode));
                darkModeToggle.setAttribute('aria-label', isDarkMode ? 'Switch to light mode' : 'Switch to dark mode');
            }

            if (persistPreference) {
                localStorage.setItem('darkMode', String(isDarkMode));
                localStorage.setItem('sithMode', 'false');
            }
        }

        function applySithMode(enabled, persistPreference = true) {
            const body = document.body;
            if (!body) return;
            body.classList.toggle('dark-mode', enabled);
            body.classList.toggle('sith-mode', enabled);
            const toggleIcon = document.getElementById('toggleIcon');
            if (toggleIcon) {
                toggleIcon.src = enabled ? '/assets/CADBayDark.png' : '/assets/CADBayLight.png';
                toggleIcon.onerror = () => {
                    toggleIcon.onerror = null;
                    toggleIcon.src = enabled ? '/assets/CADBayDark.webp' : '/assets/CADBayLight.webp';
                };
                toggleIcon.classList.toggle('dark', enabled);
                toggleIcon.classList.toggle('light', !enabled);
                toggleIcon.alt = enabled ? 'Sith mode' : 'Light mode';
            }
            const darkModeToggle = document.getElementById('darkModeToggle');
            if (darkModeToggle) {
                darkModeToggle.setAttribute('aria-pressed', String(enabled));
                darkModeToggle.setAttribute('aria-label', enabled ? 'Exit Sith mode' : 'Switch to dark mode');
            }
            if (persistPreference) {
                localStorage.setItem('sithMode', String(enabled));
                localStorage.setItem('darkMode', String(enabled));
            }
        }

        window.applyThemePreference = applyThemePreference;
        window.applySithMode = applySithMode;

        function showSithContextMenu(event) {
            event.preventDefault();
            document.querySelector('.sith-context-menu')?.remove();
            const menu = document.createElement('div');
            menu.className = 'sith-context-menu';
            menu.style.left = `${Math.min(event.clientX, window.innerWidth - 190)}px`;
            menu.style.top = `${Math.min(event.clientY, window.innerHeight - 55)}px`;
            const action = document.createElement('button');
            const enabled = document.body.classList.contains('sith-mode');
            action.type = 'button';
            action.textContent = enabled ? 'Exit Sith mode' : 'Enter Sith mode';
            action.addEventListener('click', () => {
                applySithMode(!enabled);
                menu.remove();
            });
            menu.appendChild(action);
            document.body.appendChild(menu);
            setTimeout(() => document.addEventListener('click', () => menu.remove(), { once: true }), 0);
        }

        // Dark mode and header behavior logic
        function initializeDarkMode() {
            console.log('initializeDarkMode running');
            const darkModeToggle = document.getElementById('darkModeToggle');
            const toggleIcon = document.getElementById('toggleIcon');
            const body = document.body;

            if (!darkModeToggle || !toggleIcon) {
                console.log('dark mode elements missing, aborting');
                return;
            }

            toggleIcon.src = '/assets/CADBayLight.png';
            toggleIcon.onerror = () => {
                toggleIcon.onerror = null;
                toggleIcon.src = '/assets/CADBayLight.webp';
            };
            toggleIcon.width = 160;
            toggleIcon.height = 42;
            toggleIcon.style.objectFit = 'contain';

            const savedTheme = localStorage.getItem('darkMode');
            const session = typeof getSessionData === 'function' ? getSessionData() : null;
            const accountTheme = session && session.theme ? String(session.theme).toLowerCase() : null;
            const preferredTheme = accountTheme || (savedTheme === 'true' ? 'dark' : 'light');
            console.log('restoring dark mode:', preferredTheme);
            applyThemePreference(preferredTheme, !accountTheme);
            if (!accountTheme && localStorage.getItem('sithMode') === 'true') {
                applySithMode(true, false);
            }

            // dark mode toggle listener
            darkModeToggle.addEventListener('click', function(e) {
                e.preventDefault();
                const isNowDark = !document.body.classList.contains('dark-mode');
                if (document.body.classList.contains('sith-mode')) {
                    applySithMode(false);
                    return;
                }
                applyThemePreference(isNowDark ? 'dark' : 'light', true);
                console.log('toggle clicked, now dark?', isNowDark);
            });

            document.querySelectorAll('.brand-mark, #darkModeToggle').forEach(element => {
                element.addEventListener('contextmenu', showSithContextMenu);
            });

        }
        // need to split ads between banners, sidebars, and main content
        //also need to hide ads if premium user logged in
        window.adTheme = {
            getCurrentTheme() {
                if (document.body.classList.contains('sith-mode')) return 'sith';
                return document.body.classList.contains('dark-mode') ? 'dark' : 'light';
            },
            useSiteTheme(rootId = 'ad-content') {
                const root = document.getElementById(rootId);
                if (!root) return;
                const theme = this.getCurrentTheme();
                root.classList.toggle('ad-theme-dark', theme === 'dark');
                root.classList.toggle('ad-theme-sith', theme === 'sith');
                root.classList.toggle('ad-theme-light', theme === 'light');
            },
            setTheme(theme, rootId = 'ad-content') {
                const root = document.getElementById(rootId);
                if (!root) return;
                root.classList.toggle('ad-theme-dark', theme === 'dark');
                root.classList.toggle('ad-theme-sith', theme === 'sith');
                root.classList.toggle('ad-theme-light', theme === 'light');
            }
        };

        async function assetExists(path) {
            try {
                const response = await fetch(path, { method: 'HEAD' });
                if (response.ok) return true;
            } catch (e) {
                // HEAD may not be allowed in some environments, fall back to GET for existence check.
            }
            try {
                const response = await fetch(path, { method: 'GET', headers: { 'Range': 'bytes=0-1' } });
                return response.ok;
            } catch (e) {
                return false;
            }
        }

        async function parseConf(path) {
            try {
                const response = await fetch(path);
                if (!response.ok) return null;
                const text = await response.text();
                const config = {};
                text.split(/\r?\n/).forEach(line => {
                    const trimmed = line.trim();
                    if (!trimmed || trimmed.startsWith('#')) return;

                    let key, value;
                    if (trimmed.includes('=')) {
                        [key, ...value] = trimmed.split('=');
                        value = value.join('=').trim();
                    } else if (trimmed.includes(':')) {
                        [key, ...value] = trimmed.split(':');
                        value = value.join(':').trim();
                    } else {
                        // bare URL line as shorthand for link
                        key = 'link';
                        value = trimmed;
                    }

                    if (!key || !value) return;
                    key = key.trim().toLowerCase();
                    if (key === 'desc') key = 'description';
                    config[key] = value;
                });
                return config;
            } catch (e) {
                return null;
            }
        }


        function getFileCount(folder) {
            let count = 0;
            while (true) {
                const subfolder = `${folder}/${count + 1}`;
                if (!assetExists(subfolder)) break;
                count++;
            }
            return count;
        }
        async function resolveAdSource(ad) {
            const path = typeof ad === 'string' ? ad : ad && ad.path;
            const normalizedPath = `/${String(path || '').replace(/^\/+/, '')}`;
            const normalizedFolder = normalizedPath.substring(0, normalizedPath.lastIndexOf('/'));
            try {
                if (ad && typeof ad === 'object' && ad.type === 'html') {
                    return { type: 'html', path: normalizedPath, dir: `${normalizedFolder}/` };
                }
                if (ad && typeof ad === 'object' && ad.type === 'image') {
                    const config = ad.meta ? await parseConf(ad.meta) : {};
                    return {
                        type: 'image',
                        path: normalizedPath,
                        link: config.link || '#',
                        description: config.description || 'Advertisement'
                    };
                }

                const htmlResponse = await fetch(`${normalizedPath}/index.html`, { cache: 'no-store' });
                if (htmlResponse.ok) {
                    return { type: 'html', path: `${normalizedPath}/index.html`, dir: `${normalizedPath}/` };
                }

                const config = await parseConf(`${normalizedPath}/conf.conf`);
                if (!config || !config.image) return null;
                return {
                    type: 'image',
                    path: new URL(config.image, `${window.location.origin}${normalizedPath}/`).pathname,
                    link: config.link || '#',
                    description: config.description || 'Advertisement'
                };
            } catch (error) {
                return null;
            }
        }

        async function findAvailableAds(type) {
            if (!['banner', 'block', 'sidebar'].includes(type)) return [];
            try {
                const response = await fetch(`/api/ads?type=${encodeURIComponent(type)}`, {
                    cache: 'no-store'
                });
                if (!response.ok) {
                    throw new Error(`Ad index request failed with status ${response.status}`);
                }
                const ads = await response.json();
                if (!Array.isArray(ads)) {
                    throw new Error('Ad index response was not an array');
                }
                return ads.filter(ad => ad && typeof ad.path === 'string');
            } catch (error) {
                console.error(`Failed to discover ${type} ads`, error);
                return [];
            }
        }

        async function findAdsWithFallback(type) {
            return findAvailableAds(type);
        }

        async function loadAdInto(containerId, type) {
            const root = document.getElementById(containerId);
            if (!root) return;
            if (!refreshAdVisibility()) return;
            root.hidden = false;
            root.textContent = 'Loading ad...';
            const folders = await findAdsWithFallback(type);
            if (!areAdsEnabled(getStoredSessionData())) {
                setAdsVisible(false);
                return;
            }
            if (!folders.length) {
                root.hidden = true;
                root.replaceChildren();
                return;
            }
            const source = await resolveAdSource(folders[Math.floor(Math.random() * folders.length)]);
            if (!source) {
                root.hidden = true;
                root.replaceChildren();
                return;
            }
            root.hidden = false;
            root.replaceChildren();
            renderAdSource(source, root);
            if (window.adTheme && typeof window.adTheme.useSiteTheme === 'function') {
                window.adTheme.useSiteTheme(containerId);
            }
        }

        function renderAdSource(source, adRoot) {
            if (!source || !adRoot) return;
            if (!areAdsEnabled(getStoredSessionData())) {
                setAdsVisible(false);
                return;
            }
            if (source.type === 'html') {
                fetch(source.path)
                    .then(response => {
                        if (!response.ok) {
                            throw new Error('Ad file not found: ' + source.path);
                        }
                        return response.text();
                    })
                    .then(html => {
                        if (!areAdsEnabled(getStoredSessionData())) {
                            setAdsVisible(false);
                            return;
                        }
                        const tmp = document.createElement('div');
                        tmp.innerHTML = html;

                        tmp.querySelectorAll('link[rel="stylesheet"]').forEach(link => {
                            const href = link.getAttribute('href');
                            if (href && !href.startsWith('http') && !href.startsWith('/')) {
                                link.href = source.dir + href;
                            }
                            document.head.appendChild(link);
                            link.remove();
                        });

                        tmp.querySelectorAll('script').forEach(old => {
                            const script = document.createElement('script');
                            const src = old.getAttribute('src');
                            if (src) {
                                script.src = src.startsWith('http') || src.startsWith('/') ? src : source.dir + src;
                            } else {
                                script.textContent = old.textContent;
                            }
                            document.head.appendChild(script);
                            old.remove();
                        });

                        adRoot.innerHTML = tmp.innerHTML;
                    })
                    .catch(e => {
                        console.error('Failed to load HTML ad', e);
                        adRoot.hidden = true;
                        adRoot.replaceChildren();
                    });
            } else if (source.type === 'image') {
                adRoot.innerHTML = '';
                const link = document.createElement('a');
                link.href = source.link || '404.html';
                link.style.display = 'block';
                link.style.textDecoration = 'none';
                link.style.color = 'inherit';

                const img = document.createElement('img');
                img.src = source.path;
                img.alt = source.description || 'Advertisement';
                img.style.width = '100%';
                img.style.height = 'auto';
                img.style.display = 'block';
                img.style.borderRadius = '8px';
                img.style.cursor = 'pointer';

                link.appendChild(img);
                adRoot.appendChild(link);
            }
        }

        async function loadAd(requestedAd) {
            const adRoot = document.getElementById('ad-content');
            const adPanel = document.getElementById('ad-panel');
            if (!adRoot) return;
            if (!refreshAdVisibility()) return;
            adRoot.hidden = false;
            adRoot.textContent = 'Loading ad...';
            const requestedValue = requestedAd !== null ? requestedAd.toString().trim() : null;

            if (requestedValue !== null && requestedValue !== '' && Number(requestedValue) === 0) {
                if (adPanel) {
                    adPanel.style.display = 'none';
                }
                adRoot.hidden = true;
                adRoot.replaceChildren();
                return;
            }

            // Try to load an ad based on type
            try {
                // Try to load a block ad first (the main type)
                const blockAds = await findAdsWithFallback('block');
                if (blockAds.length > 0) {
                    const randomIndex = Math.floor(Math.random() * blockAds.length);
                    const folder = blockAds[randomIndex];
                    renderAdSource(await resolveAdSource(folder), adRoot);
                } else {
                    // Fallback to banner ads
                    const bannerAds = await findAvailableAds('banner');
                    if (bannerAds.length > 0) {
                        const randomIndex = Math.floor(Math.random() * bannerAds.length);
                        const folder = bannerAds[randomIndex];
                        renderAdSource(await resolveAdSource(folder), adRoot);
                    } else {
                        // Fallback to sidebar ads
                        const sidebarAds = await findAvailableAds('sidebar');
                        if (sidebarAds.length > 0) {
                            const randomIndex = Math.floor(Math.random() * sidebarAds.length);
                            const folder = sidebarAds[randomIndex];
                            renderAdSource(await resolveAdSource(folder), adRoot);
                        } else {
                            adRoot.hidden = true;
                            adRoot.replaceChildren();
                        }
                    }
                }
            } catch (error) {
                console.error('Failed to load any ad:', error);
                adRoot.hidden = true;
                adRoot.replaceChildren();
            }
        }
