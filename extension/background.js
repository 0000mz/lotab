// Helper to log events with timestamp
const logEvent = (eventName, data) => {
    console.log(`[${new Date().toISOString()}] ${eventName}`, data);
};

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
