function shuffle(items, random = Math.random) {
    const result = items.slice();
    for (let index = result.length - 1; index > 0; index -= 1) {
        const swapIndex = Math.floor(random() * (index + 1));
        [result[index], result[swapIndex]] = [result[swapIndex], result[index]];
    }
    return result;
}

function normalizeTier(tier) {
    if (tier === true || tier === 'true') return 'premium';
    const normalized = String(tier || '').trim().toLowerCase();
    if (['premium', 'dev', 'developer', 'moderator', 'insider'].includes(normalized)) return 'premium';
    if (normalized === 'enterprise') return 'enterprise';
    if (normalized === 'ad') return 'ad';
    return 'free';
}

function quotasForTier(userTier) {
    switch (normalizeTier(userTier)) {
        case 'enterprise': return { ad: 0, enterprise: 2, free: 3, premium: 5 };
        case 'premium': return { ad: 0, enterprise: 3, free: 3, premium: 4 };
        default: return { ad: 2, enterprise: 3, free: 2, premium: 3 };
    }
}

function pullRfps(userTier, rfps, random = Math.random) {
    if (!Array.isArray(rfps)) return [];
    const quotas = quotasForTier(userTier);
    const selected = [];
    for (const tier of Object.keys(quotas)) {
        const matches = rfps.filter(rfp => rfp && normalizeTier(rfp.tier) === tier);
        selected.push(...shuffle(matches, random).slice(0, quotas[tier]));
    }
    return shuffle(selected, random);
}

function individualRfpPull(tier, rfps, random = Math.random) {
    const available = Array.isArray(rfps) ? rfps : [];
    const normalizedTier = normalizeTier(tier);
    const fallback = { ad: ['free', 'premium'], enterprise: ['premium', 'free'], free: ['premium', 'enterprise'], premium: ['enterprise', 'free'] };
    const tiers = [...new Set([normalizedTier, ...(fallback[normalizedTier] || [])])];
    for (const candidateTier of tiers) {
        const matches = available.filter(rfp => rfp && normalizeTier(rfp.tier) === candidateTier);
        if (matches.length) return matches[Math.floor(random() * matches.length)];
    }
    return null;
}

function readRfp(rfpId, tier, fsModule = require('fs'), root = 'RFPs') {
    const normalizedTier = normalizeTier(tier);
    if (!/^[A-Za-z0-9_-]+$/.test(String(rfpId || ''))) return null;
    if (!['ad', 'enterprise', 'free', 'premium'].includes(normalizedTier)) return null;
    const path = require('path').join(root, normalizedTier, `${rfpId}.txt`);
    try {
        const fields = {};
        for (const line of fsModule.readFileSync(path, 'utf8').split(/\r?\n/)) {
            const separator = line.indexOf(':');
            if (separator > 0) {
                const key = line.slice(0, separator).trim().toLowerCase().replace(/\s+/g, '_');
                fields[key] = line.slice(separator + 1).trim();
            }
        }
        return { ...fields, tier: normalizeTier(fields.tier || normalizedTier) };
    } catch (error) {
        return null;
    }
}

function loadRfps(root = 'RFPs', fsModule = require('fs'), pathModule = require('path')) {
    const rfps = [];
    for (const tier of ['ad', 'enterprise', 'free', 'premium']) {
        const directory = pathModule.join(root, tier);
        let files;
        try {
            files = fsModule.readdirSync(directory, { withFileTypes: true });
        } catch (error) {
            continue;
        }
        for (const file of files) {
            if (tier === 'ad' && file.isDirectory()) {
                rfps.push({
                    id: file.name,
                    tier: 'ad',
                    title: file.name,
                    path: pathModule.join(directory, file.name),
                    source: 'ad'
                });
                continue;
            }
            if (!file.isFile() || !/\.txt$/i.test(file.name)) continue;
            const id = file.name.replace(/\.txt$/i, '');
            const record = readRfp(id, tier, {
                readFileSync: filePath => fsModule.readFileSync(filePath, 'utf8')
            }, root);
            if (record) rfps.push({ ...record, id, tier });
        }
    }
    return rfps;
}

function selectRfps(userTier, options = {}) {
    const rfps = Array.isArray(options.rfps)
        ? options.rfps
        : loadRfps(options.root, options.fsModule, options.pathModule);
    return pullRfps(userTier, rfps, options.random || Math.random);
}

const api = {
    shuffle,
    normalizeTier,
    quotasForTier,
    pullRfps,
    individualRfpPull,
    readRfp,
    loadRfps,
    selectRfps
};
if (typeof module !== 'undefined') module.exports = api;
if (typeof window !== 'undefined') window.CADBayRfpAlgorithm = api;
