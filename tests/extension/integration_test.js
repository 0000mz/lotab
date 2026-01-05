const puppeteer = require('puppeteer');
const WebSocket = require('ws');
const path = require('path');
const { expect } = require('chai');

const EXTENSION_PATH = path.resolve(__dirname, '../../extension');
const WS_PORT = 9001;
const TEST_TIMEOUT = 10000;

function log(msg) {
    console.log(`[Test] ${msg}`);
}

class TestContext {
    constructor() {
        this.wss = null;
        this.browser = null;
        this.ws = null;
        this.messageHandlers = new Set();
    }

    async start() {
        log('Starting Mock WebSocket Server...');
        this.wss = new WebSocket.Server({ port: WS_PORT });

        const connectionPromise = new Promise((resolve) => {
            this.wss.on('connection', (ws) => {
                log('Client connected!');
                this.ws = ws;
                ws.on('message', (message) => {
                    // log(`Received: ${message}`); // Optional: reduce noise
                    this._handleMessage(message);
                });
                resolve(ws);
            });
        });

        const startTime = Date.now();
        log(`Launching Chrome with extension from: ${EXTENSION_PATH}`);
        this.browser = await puppeteer.launch({
            headless: false,
            args: [
                `--disable-extensions-except=${EXTENSION_PATH}`,
                `--load-extension=${EXTENSION_PATH}`,
                '--no-sandbox',
                '--disable-setuid-sandbox'
            ]
        });

        log('Waiting for extension to connect...');
        try {
            await this.withTimeout(connectionPromise, TEST_TIMEOUT);
            const duration = Date.now() - startTime;
            log(`SUCCESS: Extension connected to WebSocket server in ${duration}ms.`);
        } catch (e) {
            await this.cleanup();
            throw e;
        }
    }

    _handleMessage(message) {
        try {
            const data = JSON.parse(message);
            for (const handler of this.messageHandlers) {
                if (handler(data)) {
                    this.messageHandlers.delete(handler);
                    break;
                }
            }
        } catch (e) {
            console.error('Failed to parse message:', e);
        }
    }

    async sendMessageAndWait(message, predicate, timeout = 5000) {
        return new Promise(async (resolve, reject) => {
            const handler = (data) => {
                if (predicate(data)) {
                    resolve(data);
                    return true;
                }
                return false;
            };

            this.messageHandlers.add(handler);

            if (this.ws && this.ws.readyState === WebSocket.OPEN) {
                this.ws.send(JSON.stringify(message));
            } else {
                this.messageHandlers.delete(handler);
                reject(new Error('WebSocket not connected'));
            }

            // Timeout for waiting response
            setTimeout(() => {
                if (this.messageHandlers.has(handler)) {
                    this.messageHandlers.delete(handler);
                    reject(new Error(`Timeout waiting for response matching predicate: ${predicate}`));
                }
            }, timeout);
        });
    }

    withTimeout(promise, ms) {
        let timeoutHandle;
        const timeoutPromise = new Promise((_, reject) => {
            timeoutHandle = setTimeout(() => {
                reject(new Error(`Operation timed out after ${ms}ms`));
            }, ms);
        });

        return Promise.race([promise, timeoutPromise]).finally(() => {
            clearTimeout(timeoutHandle);
        });
    }

    async cleanup() {
        if (this.browser) await this.browser.close();
        if (this.wss) this.wss.close();
    }
}

describe('Lotab Extension Integration', function () {
    this.timeout(20000); // 20 seconds global timeout for browser launch etc.
    let ctx;

    before(async function () {
        ctx = new TestContext();
        await ctx.start();
    });

    after(async function () {
        if (ctx) await ctx.cleanup();
    });

    it('should have an active WebSocket connection', function () {
        expect(ctx.ws).to.not.be.null;
        expect(ctx.ws.readyState).to.equal(WebSocket.OPEN);
    });

    it('should return all tabs when requested', async function () {
        const response = await ctx.sendMessageAndWait(
            { event: 'Daemon::WS::AllTabsInfoRequest' },
            (data) => data.event === 'Extension::WS::AllTabsInfoResponse'
        );

        expect(response).to.have.property('data').that.is.an('array');
        if (response.data.length > 0) {
            log(`Verified ${response.data.length} tabs received`);
        } else {
            console.warn('Warning: Received empty tabs list');
        }
    });
});
