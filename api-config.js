(function configureCadbayApi(window) {
    const storageKey = 'cadbay_api_endpoint';
    const apiPath = '/api';

    function normalizeEndpoint(value) {
        if (!value || typeof value !== 'object') return null;
        const host = String(value.host || '').trim();
        const port = String(value.port || '').trim();
        if (!host || !/^[a-z0-9.-]+$/i.test(host)) return null;
        if (!/^\d+$/.test(port) || Number(port) < 1 || Number(port) > 65535) return null;
        return { host, port };
    }

    function configuredEndpoint() {
        try {
            return normalizeEndpoint(JSON.parse(window.localStorage.getItem(storageKey) || 'null'));
        } catch (error) {
            console.warn('Ignoring invalid CADBay API endpoint configuration:', error);
            return null;
        }
    }

    function apiOrigin() {
        const endpoint = configuredEndpoint();
        return endpoint
            ? `${window.location.protocol}//${endpoint.host}:${endpoint.port}`
            : window.location.origin;
    }

    window.CADBAY_API_PATH = apiPath;
    window.getCadbayApiConfig = function () {
        return configuredEndpoint() || {
            host: window.location.hostname || 'localhost',
            port: window.location.port || (window.location.protocol === 'https:' ? '443' : '80')
        };
    };
    window.setCadbayApiConfig = function (config) {
        const endpoint = normalizeEndpoint(config);
        if (!endpoint) throw new Error('Enter a valid host/IP and port (1-65535).');
        window.localStorage.setItem(storageKey, JSON.stringify(endpoint));
        return endpoint;
    };
    window.clearCadbayApiConfig = function () {
        window.localStorage.removeItem(storageKey);
    };
    window.cadbayApiUrl = function (path) {
        const suffix = String(path || '').replace(/^\/+/, '');
        return `${apiOrigin()}${apiPath}/${suffix}`;
    };
})(window);
