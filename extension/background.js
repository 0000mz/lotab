let socket = null;
const DAEMON_URL = 'ws://localhost:9001';
let reconnectInterval = 1000;
let eventQueue = [];

function connectToDaemon() {
    socket = new WebSocket(DAEMON_URL);

    socket.onopen = () => {
        console.log(`[${new Date().toISOString()}] Connected to Daemon WebSocket`);
        reconnectInterval = 1000;

        // Flush queue
        while (eventQueue.length > 0 && socket.readyState === WebSocket.OPEN) {
            socket.send(JSON.stringify(eventQueue.shift()));
        }
    };

    socket.onmessage = (event) => {
        console.log(`[${new Date().toISOString()}] Message from Daemon:`, event.data);
        try {
            const message = JSON.parse(event.data);
            if (message.event === 'request_tab_info') {
                console.log('Received request_tab_info, querying tabs...');
                chrome.tabs.query({}, (tabs) => {
                    const reduced_tabs = tabs.map(t => ({
                        title: t.title,
                        id: t.id,
                        url: t.url,
                    }));
                    logEvent('tabs.onAllTabs', reduced_tabs);
                });
            }
        } catch (e) {
            console.error('Failed to parse message from daemon:', e);
        }
    };

    socket.onclose = (event) => {
        console.log(`[${new Date().toISOString()}] Daemon WebSocket closed. Reconnecting in ${reconnectInterval}ms...`, event.reason);
        setTimeout(connectToDaemon, reconnectInterval);
        reconnectInterval = Math.min(reconnectInterval * 2, 30000);
    };

    socket.onerror = (error) => {
        console.error(`[${new Date().toISOString()}] Daemon WebSocket error:`, error);
    };
}

// Helper to log events with timestamp and send to daemon
const logEvent = (eventName, data) => {
    const timestamp = new Date().toISOString();
    const eventPayload = {
        event: eventName,
        timestamp: timestamp,
        data: data
    };

    console.log(`[${timestamp}] ${eventName}`, data);

    if (socket && socket.readyState === WebSocket.OPEN) {
        socket.send(JSON.stringify(eventPayload));
    } else {
        console.log(`[${timestamp}] WebSocket not open, queuing event: ${eventName}`);
        eventQueue.push(eventPayload);
        // Limit queue size to avoid memory issues (e.g., 1000 events)
        if (eventQueue.length > 1000) {
            eventQueue.shift();
        }
    }
};

// Initial connection
connectToDaemon();

// --- Tab Events ---

chrome.tabs.onCreated.addListener((tab) => {
    logEvent('tabs.onCreated', tab);
});

chrome.tabs.onUpdated.addListener((tabId, changeInfo, tab) => {
    logEvent('tabs.onUpdated', { tabId, changeInfo, tab });
});

chrome.tabs.onRemoved.addListener((tabId, removeInfo) => {
    logEvent('tabs.onRemoved', { tabId, removeInfo });
});

chrome.tabs.onMoved.addListener((tabId, moveInfo) => {
    logEvent('tabs.onMoved', { tabId, moveInfo });
});

chrome.tabs.onActivated.addListener((activeInfo) => {
    logEvent('tabs.onActivated', activeInfo);
});

chrome.tabs.onHighlighted.addListener((highlightInfo) => {
    logEvent('tabs.onHighlighted', highlightInfo);
});

chrome.tabs.onDetached.addListener((tabId, detachInfo) => {
    logEvent('tabs.onDetached', { tabId, detachInfo });
});

chrome.tabs.onAttached.addListener((tabId, attachInfo) => {
    logEvent('tabs.onAttached', { tabId, attachInfo });
});

chrome.tabs.onReplaced.addListener((addedTabId, removedTabId) => {
    logEvent('tabs.onReplaced', { addedTabId, removedTabId });
});

chrome.tabs.onZoomChange.addListener((zoomChangeInfo) => {
    logEvent('tabs.onZoomChange', zoomChangeInfo);
});

// --- Tab Group Events ---

if (chrome.tabGroups) {
    chrome.tabGroups.onCreated.addListener((group) => {
        logEvent('tabGroups.onCreated', group);
    });

    chrome.tabGroups.onUpdated.addListener((group) => {
        logEvent('tabGroups.onUpdated', group);
    });

    chrome.tabGroups.onRemoved.addListener((group) => {
        logEvent('tabGroups.onRemoved', group);
    });

    chrome.tabGroups.onMoved.addListener((group) => {
        logEvent('tabGroups.onMoved', group);
    });
} else {
    console.log('chrome.tabGroups API not available');
}

console.log('Tab Monitor Extension Loaded');
