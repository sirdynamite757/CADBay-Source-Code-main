        loadFragment('header', 'header.html').then(() => {
            if (typeof initializeDarkMode === 'function') initializeDarkMode();
            if (typeof updateAccountMenu === 'function') updateAccountMenu();
            return loadFragment('footer', 'footer.html');
        }).then(async () => {
            const session = typeof getSessionData === 'function' ? getSessionData() : null;
            const username = document.getElementById('settings-username');
            const email = document.getElementById('settings-email');
            const phone = document.getElementById('settings-phone');
            const icon = document.getElementById('settings-icon');
            const iconPicker = document.getElementById('account-icon-picker');
            const profileMessage = document.getElementById('profile-message');
            const theme = document.getElementById('settings-theme');
            if (!session) {
                document.getElementById('settings-account-message').textContent = 'Sign in to manage account settings.';
                document.querySelectorAll('#profile-form button, #password-form button, #security-wipe').forEach(button => {
                    button.disabled = true;
                });
                return;
            }
            const settingsResponse = await fetch(cadbayApiUrl('/account-settings'), { credentials: 'include' });
            if (!settingsResponse.ok) {
                document.getElementById('settings-account-message').textContent = 'Unable to load account settings.';
                document.querySelectorAll('#profile-form button, #password-form button, #security-wipe').forEach(button => {
                    button.disabled = true;
                });
                return;
            }
            const account = await settingsResponse.json();
            document.getElementById('settings-username-display').textContent =
                account.username || session.username || session.user_id || 'Unknown';
            const usernameForm = document.getElementById('username-form');// username box needs to allow for a user to change their username, we need to add this functionality now, it should check if the username is already taken however, if the username is taken, it should display a message saying that the username is already taken and to choose another one, if the username is not taken, it should update the username in the account data and display a message saying that the username has been updated successfully

            

            email.value = account.email || '';
            phone.value = account.phone || '';


            const rawStatus = String(account.status || account.account_status || '').trim().toLowerCase();
            const accountStatus = ['dev', 'developer', 'enterprise', 'moderator', 'premium'].includes(rawStatus)
                ? rawStatus
                : 'free';
            const premium = typeof window.isPremiumActive === 'function'
                ? window.isPremiumActive(account)
                : accountStatus === 'premium' || accountStatus === 'enterprise';
            const statusText = premium
                ? `Account status: ${accountStatus === 'free' ? 'premium' : accountStatus} • expiry ${account.premium_expiry || account.premium || 'active'}`
                : `Account status: ${accountStatus}`;
            document.getElementById('premium-status').textContent = statusText;// cannot find dev, enterprise, or moderator status in the account data, we need to add this functionality now
            const accountTheme = String(account.theme || '').toLowerCase();
            theme.value = ['light', 'dark', 'sith'].includes(accountTheme)
                ? accountTheme
                : (localStorage.getItem('darkMode') === 'true' ? 'dark' : 'light');

            const devAdsSetting = document.getElementById('dev-ads-setting');
            const devAdsToggle = document.getElementById('dev-ads-toggle');
            const isDev = typeof window.isDevAccount === 'function'
                ? window.isDevAccount(account)
                : ['dev', 'developer'].includes(String(account.status || account.account_status || '').toLowerCase());
            if (devAdsSetting && devAdsToggle && isDev) {
                devAdsSetting.hidden = false;
                const getPreference = typeof window.getClientCookie === 'function'
                    ? window.getClientCookie
                    : () => null;
                devAdsToggle.checked = getPreference('cadbay_dev_ads_enabled') !== 'false';
                devAdsToggle.addEventListener('change', () => {
                    if (typeof window.setDeveloperAdsEnabled === 'function') {
                        window.setDeveloperAdsEnabled(devAdsToggle.checked);
                    } else {
                        if (typeof window.setClientCookie === 'function') {
                            window.setClientCookie('cadbay_dev_ads_enabled', devAdsToggle.checked);
                        }
                        if (typeof window.refreshAdVisibility === 'function') {
                            window.refreshAdVisibility();
                        }
                    }
                });
            }

            let currentIcon = account.icon || account.account_icon || '';

            function getNumberOfFiles(folder) {
            for (const file of folder.files) {
                if (file && typeof file === 'string' && file.trim() !== '') {
                    return folder.files.length;
                }
            }
            return 0;
            }

            function normalizeIconToken(value) {// the icons are broken again, we need to fix this by normalizing the icon token to remove any data urls or file extensions and return the basename of the icon path
                // grab the name of eachg icon in the file  alphabetically and return them to the "loadDefaultIcons" function to display them in the icon picker, we need to make sure that the icons are sorted alphabetically and that they are displayed in the correct order
                if (!value) return '';
                const url = new URL(value, window.location.origin);
                const pathname = url.pathname;
                const basename = pathname.substring(pathname.lastIndexOf('/') + 1);
                const nameWithoutExtension = basename.replace(/\.[^/.]+$/, '');
                return nameWithoutExtension.toLowerCase();
            }

            function defaultIconPathFromName(name) {
                if (!name) return '';
                const cleanName = normalizeIconToken(name);
                if (!cleanName) return '';
                return `/assets/accountIconsDefault/${cleanName}.png`;
            }

            loadDefaultIcons(currentIcon);

            async function loadDefaultIcons(selectedIcon) {
                try {
                    const iconResponse = await fetch(cadbayApiUrl('/account-icons'), { credentials: 'include' });
                    if (!iconResponse.ok) throw new Error('Icon service unavailable');
                    const defaultIcons = await iconResponse.json();
                    iconPicker.textContent = '';
                    if (!Array.isArray(defaultIcons) || defaultIcons.length === 0) {
                        iconPicker.textContent = 'No default icons are available.';
                        return;
                    }
                    const selectedToken = normalizeIconToken(selectedIcon);
                    defaultIcons.forEach(defaultIcon => {// why are the icons not loading, we need to fix this now, make all the code as simple as possible and make sure that the icons are loading correctly, we need to make sure that the icons are displayed in the correct order and that they are displayed correctly in the icon picker
                        const label = document.createElement('label');
                        const input = document.createElement('input');
                        const image = document.createElement('img');
                        const iconPath = defaultIcon.path || defaultIconPathFromName(defaultIcon.name);
                        const normalizedDefaultName = normalizeIconToken(defaultIcon.name);
                        const normalizedIconPath = normalizeIconToken(iconPath);
                        input.type = 'radio';
                        input.name = 'default_icon';
                        input.value = iconPath;
                        input.checked = Boolean(selectedToken) && (
                            normalizedDefaultName === selectedToken ||
                            normalizedIconPath === selectedToken ||
                            iconPath === selectedIcon ||
                            normalizeIconToken(defaultIcon.path) === selectedToken ||
                            normalizeIconToken(defaultIcon.name) === selectedToken
                        );
                        image.src = iconPath;
                        image.alt = `${defaultIcon.name} icon`;
                        label.append(input, image, ` ${defaultIcon.name}`);
                        iconPicker.appendChild(label);
                        input.addEventListener('change', () => {
                            if (input.checked) {
                                currentIcon = input.value;
                                icon.value = '';
                            }
                        });
                    });
                } catch (error) {
                    iconPicker.textContent = 'Default icons could not be loaded. You can upload a PNG instead.';
                }
            }

            document.getElementById('profile-form').addEventListener('submit', async event => {
                event.preventDefault();
                const selectedIcon = icon.files[0];
                if (selectedIcon && selectedIcon.size > 1000000 * 1024) {
                    profileMessage.textContent = 'Choose an image smaller than 10 MB.';
                    return;
                }
                const selectedDefault = document.querySelector('input[name="default_icon"]:checked');
                const save = iconData => updateSettings({ action: 'profile', email: email.value.trim(), phone: phone.value.trim(), account_icon: iconData });
                if (selectedIcon) {
                    const reader = new FileReader();
                    reader.onload = () => save(reader.result);
                    reader.readAsDataURL(selectedIcon);
                } else {
                    save(selectedDefault ? selectedDefault.value : currentIcon);
                }
            });

            document.getElementById('remove-icon').addEventListener('click', () => { currentIcon = ''; icon.value = ''; });
            document.getElementById('save-theme').addEventListener('click', async () => {
                const selectedTheme = theme.value;
                if (typeof window.applyThemePreference === 'function') {
                    window.applyThemePreference(selectedTheme);
                } else {
                    document.body.classList.toggle('dark-mode', selectedTheme === 'dark');
                }
                const updateTheme = await fetch(cadbayApiUrl('/account-settings'), {
                    method: 'POST',
                    credentials: 'include',
                    headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
                    body: new URLSearchParams({ action: 'theme', theme: selectedTheme })
                });
                document.getElementById('theme-message').textContent = updateTheme.ok ? 'Appearance saved.' : 'Appearance saved locally, server sync failed.';
                if (updateTheme.ok) {
                    const session = getSessionData();
                    if (session) {
                        persistLoginSession({ ...session, theme: selectedTheme });
                    }
                }
            });
            document.getElementById('password-form').addEventListener('submit', async event => {
                event.preventDefault();
                const response = await updateSettings({ action: 'password', current_password: document.getElementById('current-password').value, new_password: document.getElementById('new-password').value });
                document.getElementById('password-message').textContent = response ? 'Password changed.' : 'Password change failed.';
                if (response) event.currentTarget.reset();
            });
            document.getElementById('security-wipe').addEventListener('click', async () => {
                if (!window.confirm('Wipe stored contact and icon data from this account?')) return;
                const response = await updateSettings({ action: 'wipe' });
                document.getElementById('wipe-message').textContent = response ? 'Sensitive account data wiped.' : 'Security wipe failed.';
                if (response) { email.value = ''; phone.value = ''; icon.value = ''; currentIcon = ''; }
            });

            async function updateSettings(data) {
                const body = new URLSearchParams(data);
                    const response = await fetch(cadbayApiUrl('/account-settings'), { method: 'POST', credentials: 'include', headers: { 'Content-Type': 'application/x-www-form-urlencoded' }, body });
                if (response.ok && data.action === 'profile') {
                    currentIcon = data.account_icon;
                    const updatedSession = { ...getSessionData(), email: data.email, phone: data.phone, account_icon: data.account_icon };
                    persistLoginSession(updatedSession);
                    if (typeof updateAccountMenu === 'function') updateAccountMenu();
                    profileMessage.textContent = 'Contact settings saved.';
                }
                if (response.ok && data.action === 'theme') {
                    const session = getSessionData();
                    if (session) {
                        persistLoginSession({ ...session, theme: data.theme || 'light' });
                    }
                }
                return response.ok;
            }
        }).catch(error => console.error('Failed to load settings:', error));