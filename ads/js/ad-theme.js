window.adTheme = {
    ensureStyles() {
        if (document.getElementById('ad-theme-stylesheet')) return;
        const link = document.createElement('link');
        link.id = 'ad-theme-stylesheet';
        link.rel = 'stylesheet';
        link.href = '/ads/css/ad-theme.css';
        document.head.appendChild(link);
    },
    getCurrentTheme() {
        if (document.body.classList.contains('sith-mode')) return 'sith';
        return document.body.classList.contains('dark-mode') ? 'dark' : 'light';
    },
    useSiteTheme(rootId = 'ad-content') {
        this.ensureStyles();
        const root = document.getElementById(rootId);
        if (!root) return;
        const theme = this.getCurrentTheme();
        root.classList.toggle('ad-theme-dark', theme === 'dark');
        root.classList.toggle('ad-theme-sith', theme === 'sith');
        root.classList.toggle('ad-theme-light', theme === 'light');
    },
    setTheme(theme, rootId = 'ad-content') {
        this.ensureStyles();
        const root = document.getElementById(rootId);
        if (!root) return;
        root.classList.toggle('ad-theme-dark', theme === 'dark');
        root.classList.toggle('ad-theme-sith', theme === 'sith');
        root.classList.toggle('ad-theme-light', theme === 'light');
    }
};
