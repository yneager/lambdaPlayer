// Generated from third_party/motrix by tools/download-engine/generate-client.mjs.
// Copyright 2018-present Dr_rOot. MIT; see licenses/Motrix-LICENSE.txt.
(() => {
const DEFAULT_BYTE_UNIT_SYSTEM = "decimal";
const PAUSE_SETTLE_TIMEOUT_MS = 30_000;
class Aria2PauseState {
    pending = new Map();
    clear() {
        this.pending.clear();
    }
    getPendingPause(gid) {
        const pause = this.pending.get(gid);
        return pause && performance.now() < pause.expiresAt ? pause : undefined;
    }
    reconcile(method, params, result, sequence) {
        const gid = typeof params[0] === 'string' ? params[0] : undefined;
        switch(method){
            case 'aria2.pause':
            case 'aria2.forcePause':
                if (gid && result === gid) {
                    this.pending.set(gid, {
                        expiresAt: performance.now() + PAUSE_SETTLE_TIMEOUT_MS
                    });
                }
                break;
            case 'aria2.unpause':
            case 'aria2.remove':
            case 'aria2.forceRemove':
            case 'aria2.removeDownloadResult':
                if (gid) this.pending.delete(gid);
                break;
            case 'aria2.unpauseAll':
                this.clear();
                break;
            case 'aria2.tellStatus':
                return this.reconcileStatus(result, sequence, gid);
            case 'aria2.tellActive':
            case 'aria2.tellWaiting':
            case 'aria2.tellStopped':
                if (Array.isArray(result)) {
                    return result.map((task)=>this.reconcileStatus(task, sequence));
                }
        }
        return result;
    }
    reconcileStatus(result, sequence, requestedGid) {
        if (!result || typeof result !== 'object') return result;
        const task = result;
        const gid = task.gid ?? requestedGid;
        if (!gid || !task.status) return result;
        const pause = this.pending.get(gid);
        if (!pause) return result;
        if (performance.now() >= pause.expiresAt || task.status === 'error' || task.status === 'complete' || task.status === 'removed') {
            this.pending.delete(gid);
            return result;
        }
        if (task.status === 'paused') {
            pause.confirmedBy = Math.max(pause.confirmedBy ?? 0, sequence);
            return result;
        }
        if (pause.confirmedBy !== undefined && sequence > pause.confirmedBy) {
            this.pending.delete(gid);
            return result;
        }
        const paused = {
            ...task,
            status: 'paused'
        };
        for (const key of [
            'downloadSpeed',
            'uploadSpeed',
            'connections'
        ]){
            if (key in task) paused[key] = '0';
        }
        return paused;
    }
}

class JsonRpcProtocol {
    transport;
    nextId = 1;
    pending = new Map();
    notificationHandler = null;
    timeoutMs;
    constructor(transport, options = {}){
        this.transport = transport;
        this.timeoutMs = options.timeoutMs ?? 10_000;
        this.transport.onMessage((data)=>this.handleMessage(data));
    }
    call(method, params) {
        const id = String(this.nextId++);
        const request = {
            jsonrpc: '2.0',
            id,
            method,
            params
        };
        return new Promise((resolve, reject)=>{
            const timer = setTimeout(()=>{
                this.pending.delete(id);
                reject(new Error(`JSON-RPC call "${method}" timed out after ${this.timeoutMs}ms`));
            }, this.timeoutMs);
            this.pending.set(id, {
                resolve: resolve,
                reject,
                timer
            });
            this.transport.send(JSON.stringify(request));
        });
    }
    multicall(calls) {
        const entries = calls.map((c)=>({
                methodName: c.method,
                params: c.params
            }));
        return this.call('system.multicall', [
            entries
        ]).then((results)=>results.map((r)=>r[0]));
    }
    multicallSettled(calls) {
        const entries = calls.map((c)=>({
                methodName: c.method,
                params: c.params
            }));
        return this.call('system.multicall', [
            entries
        ]).then((results)=>results.map((r)=>Array.isArray(r) ? {
                    status: 'fulfilled',
                    value: r[0]
                } : {
                    status: 'rejected',
                    reason: new Error(r.message)
                }));
    }
    onNotification(handler) {
        this.notificationHandler = handler;
    }
    handleMessage(data) {
        let msg;
        try {
            msg = JSON.parse(data);
        } catch  {
            return;
        }
        if (msg.id !== undefined) {
            const pending = this.pending.get(msg.id);
            if (!pending) return;
            clearTimeout(pending.timer);
            this.pending.delete(msg.id);
            if (msg.error) {
                pending.reject(new Error(msg.error.message));
            } else {
                pending.resolve(msg.result);
            }
            return;
        }
        if (msg.method) {
            this.notificationHandler?.(msg.method, msg.params ?? []);
        }
    }
}

const SECRET_EXEMPT_METHODS = new Set([
    'system.listMethods',
    'system.listNotifications'
]);
class Aria2RpcClient {
    transport;
    protocol;
    secret;
    notificationHandlers = new Map();
    pauseState = new Aria2PauseState();
    requestSequence = 0;
    constructor(transport, protocol, secret){
        this.transport = transport;
        this.protocol = protocol;
        this.secret = secret;
        this.protocol.onNotification((method, params)=>{
            this.handleNotification(method, params);
        });
    }
    async connect(port, retries = 10, delayMs = 500) {
        this.pauseState.clear();
        const url = `ws://127.0.0.1:${port}/jsonrpc`;
        for(let attempt = 0; attempt < retries; attempt++){
            try {
                await this.transport.connect(url);
                return;
            } catch (err) {
                if (attempt === retries - 1) throw err;
                await new Promise((r)=>setTimeout(r, delayMs));
            }
        }
    }
    disconnect() {
        this.pauseState.clear();
        this.transport.disconnect();
    }
    isConnected() {
        return this.transport.isConnected();
    }
    getConnectionStatus() {
        return {
            transport: 'websocket',
            connected: this.transport.isConnected()
        };
    }
    setSecret(secret) {
        this.secret = secret;
    }
    withSecret(params) {
        return this.secret === '' ? params : [
            `token:${this.secret}`,
            ...params
        ];
    }
    async call(method, params) {
        const sequence = ++this.requestSequence;
        const finalParams = SECRET_EXEMPT_METHODS.has(method) ? params : this.withSecret(params);
        const result = await this.protocol.call(method, finalParams);
        return this.pauseState.reconcile(method, params, result, sequence);
    }
    addUri(uris, options, position) {
        const params = [
            uris
        ];
        if (options !== undefined) params.push(options);
        if (position !== undefined) params.push(position);
        return this.call('aria2.addUri', params);
    }
    addUriWithCookies(uris, cookies, options, position) {
        const params = [
            uris,
            cookies
        ];
        if (options !== undefined || position !== undefined) params.push(options ?? {});
        if (position !== undefined) params.push(position);
        return this.call('aria2.addUriWithCookies', params);
    }
    addTorrent(torrentBase64, uris, options) {
        const params = [
            torrentBase64
        ];
        if (uris !== undefined) params.push(uris);
        if (options !== undefined) params.push(options);
        return this.call('aria2.addTorrent', params);
    }
    addMetalink(metalinkBase64, options) {
        const params = [
            metalinkBase64
        ];
        if (options !== undefined) params.push(options);
        return this.call('aria2.addMetalink', params);
    }
    remove(gid) {
        return this.call('aria2.remove', [
            gid
        ]);
    }
    forceRemove(gid) {
        return this.call('aria2.forceRemove', [
            gid
        ]);
    }
    pause(gid) {
        return this.call('aria2.pause', [
            gid
        ]);
    }
    forcePause(gid) {
        return this.call('aria2.forcePause', [
            gid
        ]);
    }
    async unpause(gid) {
        const pause = this.pauseState.getPendingPause(gid);
        const wasUnconfirmed = pause?.confirmedBy === undefined;
        for(;;){
            try {
                return await this.call('aria2.unpause', [
                    gid
                ]);
            } catch (error) {
                if (!pause || !wasUnconfirmed || pause !== this.pauseState.getPendingPause(gid) || !(error instanceof Error) || error.message !== `GID#${gid} cannot be unpaused now`) {
                    throw error;
                }
                await new Promise((resolve)=>setTimeout(resolve, 100));
                if (pause !== this.pauseState.getPendingPause(gid)) throw error;
            }
        }
    }
    pauseAll() {
        return this.call('aria2.pauseAll', []);
    }
    unpauseAll() {
        return this.call('aria2.unpauseAll', []);
    }
    removeDownloadResult(gid) {
        return this.call('aria2.removeDownloadResult', [
            gid
        ]);
    }
    changePosition(gid, pos, how) {
        return this.call('aria2.changePosition', [
            gid,
            pos,
            how
        ]);
    }
    tellStatus(gid, keys) {
        const params = [
            gid
        ];
        if (keys !== undefined) params.push(keys);
        return this.call('aria2.tellStatus', params);
    }
    tellActive(keys) {
        const params = [];
        if (keys !== undefined) params.push(keys);
        return this.call('aria2.tellActive', params);
    }
    tellWaiting(offset, num, keys) {
        const params = [
            offset,
            num
        ];
        if (keys !== undefined) params.push(keys);
        return this.call('aria2.tellWaiting', params);
    }
    tellStopped(offset, num, keys) {
        const params = [
            offset,
            num
        ];
        if (keys !== undefined) params.push(keys);
        return this.call('aria2.tellStopped', params);
    }
    getFiles(gid) {
        return this.call('aria2.getFiles', [
            gid
        ]);
    }
    getUris(gid) {
        return this.call('aria2.getUris', [
            gid
        ]);
    }
    getPeers(gid) {
        return this.call('aria2.getPeers', [
            gid
        ]);
    }
    getGlobalStat() {
        return this.call('aria2.getGlobalStat', []);
    }
    getGlobalOption() {
        return this.call('aria2.getGlobalOption', []);
    }
    changeGlobalOption(options) {
        return this.call('aria2.changeGlobalOption', [
            options
        ]);
    }
    getOption(gid) {
        return this.call('aria2.getOption', [
            gid
        ]);
    }
    changeOption(gid, options) {
        return this.call('aria2.changeOption', [
            gid,
            options
        ]);
    }
    getVersion() {
        return this.call('aria2.getVersion', []);
    }
    getSessionInfo() {
        return this.call('aria2.getSessionInfo', []);
    }
    shutdown() {
        return this.call('aria2.shutdown', []);
    }
    forceShutdown() {
        return this.call('aria2.forceShutdown', []);
    }
    saveSession() {
        return this.call('aria2.saveSession', []);
    }
    listMethods() {
        return this.call('system.listMethods', []);
    }
    listNotifications() {
        return this.call('system.listNotifications', []);
    }
    async multicall(calls) {
        const sequence = ++this.requestSequence;
        const withSecret = calls.map((c)=>({
                method: c.method,
                params: SECRET_EXEMPT_METHODS.has(c.method) ? c.params : this.withSecret(c.params)
            }));
        const results = await this.protocol.multicall(withSecret);
        return results.map((result, index)=>this.pauseState.reconcile(calls[index].method, calls[index].params, result, sequence));
    }
    async multicallSettled(calls) {
        const sequence = ++this.requestSequence;
        const withSecret = calls.map((c)=>({
                method: c.method,
                params: SECRET_EXEMPT_METHODS.has(c.method) ? c.params : this.withSecret(c.params)
            }));
        const results = await this.protocol.multicallSettled(withSecret);
        return results.map((result, index)=>result.status === 'fulfilled' ? {
                status: 'fulfilled',
                value: this.pauseState.reconcile(calls[index].method, calls[index].params, result.value, sequence)
            } : result);
    }
    getDownloadResultCount(filter) {
        const params = [];
        if (filter !== undefined) params.push(filter);
        return this.call('aria2.getDownloadResultCount', params);
    }
    searchDownloadResult(query, offset, num, keys) {
        const params = [
            query,
            offset,
            num
        ];
        if (keys !== undefined) params.push(keys);
        return this.call('aria2.searchDownloadResult', params);
    }
    getCheckpointStatus(outputPath) {
        return this.call('aria2.getCheckpointStatus', [
            outputPath
        ]);
    }
    exportSession(filePath) {
        return this.call('aria2.exportSession', [
            filePath
        ]);
    }
    requeueDownloadResult(gid, options) {
        const params = [
            gid
        ];
        if (options !== undefined) params.push(options);
        return this.call('aria2.requeueDownloadResult', params);
    }
    onDownloadStart(handler) {
        return this.addHandler('aria2.onDownloadStart', handler);
    }
    onDownloadPause(handler) {
        return this.addHandler('aria2.onDownloadPause', handler);
    }
    onDownloadStop(handler) {
        return this.addHandler('aria2.onDownloadStop', handler);
    }
    onDownloadComplete(handler) {
        return this.addHandler('aria2.onDownloadComplete', handler);
    }
    onDownloadError(handler) {
        return this.addHandler('aria2.onDownloadError', handler);
    }
    onBtDownloadComplete(handler) {
        return this.addHandler('aria2.onBtDownloadComplete', handler);
    }
    addHandler(method, handler) {
        if (!this.notificationHandlers.has(method)) {
            this.notificationHandlers.set(method, new Set());
        }
        const handlers = this.notificationHandlers.get(method);
        handlers?.add(handler);
        let subscribed = true;
        return ()=>{
            if (!subscribed) return;
            subscribed = false;
            handlers?.delete(handler);
            if (handlers?.size === 0) {
                this.notificationHandlers.delete(method);
            }
        };
    }
    handleNotification(method, params) {
        const handlers = this.notificationHandlers.get(method);
        if (handlers && params.length > 0) {
            const event = params[0];
            for (const handler of [
                ...handlers
            ]){
                handler(event);
            }
        }
    }
}

const UNITS = {
    decimal: [
        'B',
        'KB',
        'MB',
        'GB',
        'TB',
        'PB',
        'EB'
    ],
    binary: [
        'B',
        'KiB',
        'MiB',
        'GiB',
        'TiB',
        'PiB',
        'EiB'
    ]
};
function toByteCount(value) {
    if (typeof value === 'bigint') return value > 0n ? value : 0n;
    if (typeof value === 'number') {
        if (!Number.isFinite(value) || value <= 0) return 0n;
        return BigInt(Math.floor(value));
    }
    if (!/^\d+$/.test(value)) return 0n;
    return BigInt(value);
}
function formatByteParts(bytes, { unitSystem = DEFAULT_BYTE_UNIT_SYSTEM, decimals = 2 } = {}) {
    const value = toByteCount(bytes);
    const units = UNITS[unitSystem];
    const base = unitSystem === 'binary' ? 1024n : 1000n;
    let unitIndex = 0;
    let unitSize = 1n;
    while(unitIndex < units.length - 1 && value >= unitSize * base){
        unitIndex += 1;
        unitSize *= base;
    }
    if (unitIndex === 0) return {
        number: value.toString(),
        unit: 'B'
    };
    const scale = 10n ** BigInt(decimals);
    let rounded = (value * scale + unitSize / 2n) / unitSize;
    if (rounded >= base * scale && unitIndex < units.length - 1) {
        unitIndex += 1;
        unitSize *= base;
        rounded = (value * scale + unitSize / 2n) / unitSize;
    }
    const number = decimals === 0 ? rounded.toString() : `${rounded / scale}.${(rounded % scale).toString().padStart(decimals, '0')}`;
    return {
        number,
        unit: units[unitIndex]
    };
}
function formatBytes(bytes, options) {
    const parts = formatByteParts(bytes, options);
    return `${parts.number} ${parts.unit}`;
}
function formatSpeed(bytes, unitSystem = DEFAULT_BYTE_UNIT_SYSTEM) {
    return `${formatBytes(bytes, {
        unitSystem,
        decimals: 1
    })}/s`;
}
function formatSpeedLimit(bytes, unitSystem = DEFAULT_BYTE_UNIT_SYSTEM) {
    const { number, unit } = formatByteParts(bytes, {
        unitSystem,
        decimals: 1
    });
    return `${number.replace(/\.0$/, '')} ${unit}/s`;
}

window.Motrix = { Aria2RpcClient, JsonRpcProtocol, formatBytes, formatSpeed };
})();
