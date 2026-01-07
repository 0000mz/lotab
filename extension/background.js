let socket = null;
const DAEMON_URL = 'ws://localhost:9001';
let reconnect_interval_ms = 1000;
let event_queue = [];

// Helper to safely close tabs (handling active tab switch)
async function handleSafeClose(idsToClose) {
    const idsSet = new Set(idsToClose);

    const activeTabs = await new Promise(resolve => chrome.tabs.query({ active: true }, resolve));
    const activeTabsToClose = activeTabs.filter(t => idsSet.has(t.id));

    for (const tab of activeTabsToClose) {
        // Find other tabs in the same window
        const windowTabs = await new Promise(resolve => chrome.tabs.query({ windowId: tab.windowId }, resolve));
        const candidates = windowTabs.filter(t => !idsSet.has(t.id));

        if (candidates.length > 0) {
            // Find nearest adjacent tab
            // Prioritize: explicit next, then explicit prev
            // Given candidates are sorted by index usually, or we sort them.
            candidates.sort((a, b) => a.index - b.index);

            let best = null;
            // Try to find one immediately after
            best = candidates.find(t => t.index > tab.index);

            // If none after, find one immediately before (last one before)
            if (!best) {
                const before = candidates.filter(t => t.index < tab.index);
                if (before.length > 0) {
                    best = before[before.length - 1];
                }
            }

            // Fallback (should be covered above unless list empty)
            if (!best && candidates.length > 0) {
                best = candidates[0];
            }

            if (best) {
                console.log(`Swapping active tab from ${tab.id} to ${best.id} before close`);
                await new Promise(resolve => chrome.tabs.update(best.id, { active: true }, resolve));
            }
        }
    }

    // Now safe to remove
    chrome.tabs.remove(idsToClose, () => {
        sendAllTabs();
    });
}

// Helper to send all tabs
function sendAllTabs() {
    chrome.tabs.query({}, (tabs) => {
        chrome.tabGroups.query({}, async (groups) => {
            const reduced_tabs = await Promise.all(tabs.map(async t => ({
                title: t.title,
                id: t.id,
                url: t.url,
                active: t.active,
                groupId: t.groupId,
                browserId: (chrome.storage.session ? (await chrome.storage.session.get('browserSessionId')).browserSessionId : null)
            })));
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
    socket.onopen = async () => {
        console.log(`[${new Date().toISOString()}] Connected to Daemon WebSocket`);

        // Get or create Browser Session ID
        // We use session storage so it persists across SW restarts but clears on browser exit
        let { browserSessionId } = await chrome.storage.session.get('browserSessionId');
        if (!browserSessionId) {
            browserSessionId = crypto.randomUUID();
            await chrome.storage.session.set({ browserSessionId });
        }

        console.log(`Registering browser session: ${browserSessionId}`);
        socket.send(JSON.stringify({
            event: 'Extension::WS::RegisterBrowser',
            data: {
                browserId: browserSessionId,
                userAgent: navigator.userAgent
            }
        }));

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
                            handleSafeClose(ids);
                        }
                    }
                    break;
                case 'Daemon::WS::GroupTabs':
                    const groupTabIds = message.data.tabIds;
                    const targetGroupId = message.data.groupId; // Can be undefined (new group) or integer
                    if (groupTabIds && groupTabIds.length > 0) {
                        // Ensure all are integers
                        const gIds = groupTabIds.map(id => parseInt(id)).filter(Number.isInteger);

                        const opts = { tabIds: gIds };
                        if (Number.isInteger(targetGroupId)) {
                            opts.groupId = targetGroupId;
                        }

                        chrome.tabs.group(opts, (newGroupId) => {
                            if (chrome.runtime.lastError) {
                                console.error("GroupTabs error:", chrome.runtime.lastError);
                            } else {
                                console.log(`Grouped tabs ${gIds} into group ${newGroupId}`);
                                sendAllTabs();
                            }
                        });
                    }
                    break;
                case 'Daemon::WS::CreateTabGroupRequest':
                    const createData = message.data;
                    const createTitle = createData.title;
                    const createColor = createData.color;
                    const createTabIds = createData.tabIds;

                    if (createTabIds && createTabIds.length > 0) {
                        const cIds = createTabIds.map(id => parseInt(id)).filter(Number.isInteger);

                        chrome.tabs.group({ tabIds: cIds }, (newGroupId) => {
                            if (chrome.runtime.lastError) {
                                console.error("CreateTabGroupRequest error (group):", chrome.runtime.lastError);
                            } else {
                                chrome.tabGroups.update(newGroupId, {
                                    title: createTitle,
                                    color: createColor
                                }, (group) => {
                                    if (chrome.runtime.lastError) {
                                        console.error("CreateTabGroupRequest error (update):", chrome.runtime.lastError);
                                    }
                                    console.log(`Created group ${newGroupId} with title "${createTitle}"`);
                                    sendAllTabs();
                                });
                            }
                        });
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
    logEvent('Extension::WS::TabMoved', { tabId, moveInfo });
});

chrome.tabs.onActivated.addListener((activeInfo) => {
    logEvent('Extension::WS::TabActivated', activeInfo);
});

chrome.tabs.onHighlighted.addListener((highlightInfo) => {
    logEvent('Extension::WS::TabHighlighted', highlightInfo);
});

chrome.tabs.onDetached.addListener((tabId, detachInfo) => {
    logEvent('Extension::WS::TabDetached', { tabId, detachInfo });
});

chrome.tabs.onAttached.addListener((tabId, attachInfo) => {
    logEvent('Extension::WS::TabAttached', { tabId, attachInfo });
});

chrome.tabs.onReplaced.addListener((addedTabId, removedTabId) => {
    logEvent('Extension::WS::TabReplaced', { addedTabId, removedTabId });
});

chrome.tabs.onZoomChange.addListener((zoomChangeInfo) => {
    logEvent('Extension::WS::TabZoomChanged', zoomChangeInfo);
});

// --- Tab Group Events ---

if (chrome.tabGroups) {
    chrome.tabGroups.onCreated.addListener((group) => {
        logEvent('Extension::WS::TabGroupCreated', group);
    });

    chrome.tabGroups.onUpdated.addListener((group) => {
        logEvent('Extension::WS::TabGroupUpdated', group);
    });

    chrome.tabGroups.onRemoved.addListener((group) => {
        logEvent('Extension::WS::TabGroupRemoved', group);
    });

    chrome.tabGroups.onMoved.addListener((group) => {
        logEvent('Extension::WS::TabGroupMoved', group);
    });
} else {
    console.log('chrome.tabGroups API not available');
}

console.log('Tab Monitor Extension Loaded');
