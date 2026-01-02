let socket = null;
const DAEMON_URL = 'ws://localhost:9001';
let reconnect_interval_ms = 1000;
let event_queue = [];

// Helper to send all tabs
function sendAllTabs() {
    chrome.tabs.query({}, (tabs) => {
        chrome.tabGroups.query({}, (groups) => {
            const reduced_tabs = tabs.map(t => ({
                title: t.title,
                id: t.id,
                url: t.url,
                active: t.active,
                groupId: t.groupId,
            }));
            const reduced_groups = groups.map(g => ({
                id: g.id,
                title: g.title,
                color: g.color,
                collapsed: g.collapsed,
            }));
            logEvent('Extension::WS::AllTabsInfoResponse', {
                tabs: reduced_tabs,
                groups: reduced_groups
            });
        });
    });
}

function connectToDaemon() {
    socket = new WebSocket(DAEMON_URL);
    socket.onopen = () => {
        console.log(`[${new Date().toISOString()}] Connected to Daemon WebSocket`);

        // Flush queue
        while (event_queue.length > 0 && socket.readyState === WebSocket.OPEN) {
            socket.send(JSON.stringify(event_queue.shift()));
        }
    };

    socket.onmessage = (event) => {
        console.log(`[${new Date().toISOString()}] Message from Daemon:`, event.data);
        try {
            const message = JSON.parse(event.data);
            switch (message.event) {
                case 'Daemon::WS::AllTabsInfoRequest':
                    console.log('Received request_tab_info, querying tabs...');
                    sendAllTabs();
                    break;
                case 'Daemon::WS::ActivateTabRequest':
                    const tabId = message.data.tabId;
                    if (tabId) {
                        console.log(`Activating tab: ${tabId}`);
                        chrome.tabs.update(tabId, { active: true });
                        chrome.tabs.get(tabId, (tab) => {
                            if (tab && tab.windowId) {
                                chrome.windows.update(tab.windowId, { focused: true });
                            }
                        });
                    }
                    break;
                case 'Daemon::WS::CloseTabsRequest':
                    const tabIds = message.data.tabIds;
                    if (tabIds && Array.isArray(tabIds)) {
                        console.log(`Closing tabs:`, tabIds);
                        // Convert to integers just in case and filter
                        const ids = tabIds.map(id => parseInt(id)).filter(id => Number.isInteger(id));
                        if (ids.length > 0) {
                            chrome.tabs.remove(ids, () => {
                                sendAllTabs();
                            });
                        }
                    }
                    break;
            }
        } catch (e) {
            console.error('Failed to parse message from daemon:', e);
        }
    };

    socket.onclose = (event) => {
        console.log(`[${new Date().toISOString()}] Daemon WebSocket closed. Reconnecting in ${reconnect_interval_ms}ms...`, event.reason);
        setTimeout(connectToDaemon, reconnect_interval_ms);
    };

    socket.onerror = (error) => {
        console.error(`[${new Date().toISOString()}] Daemon WebSocket error:`, error);
    };
}

// Helper to log events with timestamp and send to daemon
const logEvent = (eventName, data) => {
    const timestamp = new Date().toISOString();

    // Query for all active tabs (across all windows)
    chrome.tabs.query({ active: true }, (activeTabs) => {
        const activeTabIds = activeTabs ? activeTabs.map(t => t.id) : [];

        const eventPayload = {
            event: eventName,
            timestamp,
            data,
            activeTabIds
        };

        console.log(`[${timestamp}] ${eventName}`, data, "Active Tabs:", activeTabIds);

        if (socket && socket.readyState === WebSocket.OPEN) {
            socket.send(JSON.stringify(eventPayload));
        } else {
            console.log(`[${timestamp}] WebSocket not open, queuing event: ${eventName}`);
            event_queue.push(eventPayload);
            // Limit queue size to avoid memory issues (e.g., 1000 events)
            if (event_queue.length > 1000) {
                event_queue.shift();
            }
        }
    });
};

// Initial connection
connectToDaemon();

// --- Tab Events ---

chrome.tabs.onCreated.addListener((tab) => {
    logEvent('Extension::WS::TabCreated', tab);
});

chrome.tabs.onUpdated.addListener((tabId, changeInfo, tab) => {
    logEvent('Extension::WS::TabUpdated', { tabId, changeInfo, tab });
});

chrome.tabs.onRemoved.addListener((tabId, removeInfo) => {
    logEvent('Extension::WS::TabRemoved', { tabId, removeInfo });
});

chrome.tabs.onMoved.addListener((tabId, moveInfo) => {
    logEvent('tabs.onMoved', { tabId, moveInfo });
});

chrome.tabs.onActivated.addListener((activeInfo) => {
    logEvent('Extension::WS::TabActivated', activeInfo);
});

chrome.tabs.onHighlighted.addListener((highlightInfo) => {
    logEvent('Extension::WS::TabHighlighted', highlightInfo);
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
    logEvent('Extension::WS::TabZoomChanged', zoomChangeInfo);
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
