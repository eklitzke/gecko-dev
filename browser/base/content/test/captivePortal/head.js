ChromeUtils.import("resource:///modules/BrowserWindowTracker.jsm");

ChromeUtils.defineModuleGetter(this, "CaptivePortalWatcher",
  "resource:///modules/CaptivePortalWatcher.jsm");

XPCOMUtils.defineLazyServiceGetter(this, "cps",
                                   "@mozilla.org/network/captive-portal-service;1",
                                   "nsICaptivePortalService");

const CANONICAL_CONTENT = "success";
const CANONICAL_URL = "data:text/plain;charset=utf-8," + CANONICAL_CONTENT;
const CANONICAL_URL_REDIRECTED = "data:text/plain;charset=utf-8,redirected";
const PORTAL_NOTIFICATION_VALUE = "captive-portal-detected";

async function setupPrefsAndRecentWindowBehavior() {
  await SpecialPowers.pushPrefEnv({
    set: [["captivedetect.canonicalURL", CANONICAL_URL],
          ["captivedetect.canonicalContent", CANONICAL_CONTENT]],
  });
  // We need to test behavior when a portal is detected when there is no browser
  // window, but we can't close the default window opened by the test harness.
  // Instead, we deactivate CaptivePortalWatcher in the default window and
  // exclude it from BrowserWindowTracker.getTopWindow in an attempt to
  // mask its presence.
  window.CaptivePortalWatcher.uninit();
  let getTopWindowCopy = BrowserWindowTracker.getTopWindow;
  let defaultWindow = window;
  BrowserWindowTracker.getTopWindow = () => {
    let win = getTopWindowCopy();
    if (win == defaultWindow) {
      return null;
    }
    return win;
  };

  registerCleanupFunction(function cleanUp() {
    BrowserWindowTracker.getTopWindow = getTopWindowCopy;
    window.CaptivePortalWatcher.init();
  });
}

async function portalDetected() {
  Services.obs.notifyObservers(null, "captive-portal-login");
  await BrowserTestUtils.waitForCondition(() => {
    return cps.state == cps.LOCKED_PORTAL;
  }, "Waiting for Captive Portal Service to update state after portal detected.");
}

async function freePortal(aSuccess) {
  Services.obs.notifyObservers(null,
    "captive-portal-login-" + (aSuccess ? "success" : "abort"));
  await BrowserTestUtils.waitForCondition(() => {
    return cps.state != cps.LOCKED_PORTAL;
  }, "Waiting for Captive Portal Service to update state after portal freed.");
}

// If a window is provided, it will be focused. Otherwise, a new window
// will be opened and focused.
async function focusWindowAndWaitForPortalUI(aLongRecheck, win) {
  // CaptivePortalWatcher triggers a recheck when a window gains focus. If
  // the time taken for the check to complete is under PORTAL_RECHECK_DELAY_MS,
  // a tab with the login page is opened and selected. If it took longer,
  // no tab is opened. It's not reliable to time things in an async test,
  // so use a delay threshold of -1 to simulate a long recheck (so that any
  // amount of time is considered excessive), and a very large threshold to
  // simulate a short recheck.
  Services.prefs.setIntPref("captivedetect.portalRecheckDelayMS", aLongRecheck ? -1 : 1000000);

  if (!win) {
    win = await BrowserTestUtils.openNewBrowserWindow();
  }
  await SimpleTest.promiseFocus(win);

  // After a new window is opened, CaptivePortalWatcher asks for a recheck, and
  // waits for it to complete. We need to manually tell it a recheck completed.
  await BrowserTestUtils.waitForCondition(() => {
    return win.CaptivePortalWatcher._waitingForRecheck;
  }, "Waiting for CaptivePortalWatcher to trigger a recheck.");
  Services.obs.notifyObservers(null, "captive-portal-check-complete");

  let notification = ensurePortalNotification(win);

  if (aLongRecheck) {
    ensureNoPortalTab(win);
    testShowLoginPageButtonVisibility(notification, "visible");
    return win;
  }

  let tab = win.gBrowser.tabs[1];
  if (tab.linkedBrowser.currentURI.spec != CANONICAL_URL) {
    // The tab should load the canonical URL, wait for it.
    await BrowserTestUtils.waitForLocationChange(win.gBrowser, CANONICAL_URL);
  }
  is(win.gBrowser.selectedTab, tab,
    "The captive portal tab should be open and selected in the new window.");
  testShowLoginPageButtonVisibility(notification, "hidden");
  return win;
}

function ensurePortalTab(win) {
  // For the tests that call this function, it's enough to ensure there
  // are two tabs in the window - the default tab and the portal tab.
  is(win.gBrowser.tabs.length, 2,
    "There should be a captive portal tab in the window.");
}

function ensurePortalNotification(win) {
  let notificationBox =
    win.document.getElementById("high-priority-global-notificationbox");
  let notification = notificationBox.getNotificationWithValue(PORTAL_NOTIFICATION_VALUE);
  isnot(notification, null,
    "There should be a captive portal notification in the window.");
  return notification;
}

// Helper to test whether the "Show Login Page" is visible in the captive portal
// notification (it should be hidden when the portal tab is selected).
function testShowLoginPageButtonVisibility(notification, visibility) {
  let showLoginPageButton = notification.querySelector("button.notification-button");
  // If the visibility property was never changed from default, it will be
  // an empty string, so we pretend it's "visible" (effectively the same).
  is(showLoginPageButton.style.visibility || "visible", visibility,
    "The \"Show Login Page\" button should be " + visibility + ".");
}

function ensureNoPortalTab(win) {
  is(win.gBrowser.tabs.length, 1,
    "There should be no captive portal tab in the window.");
}

function ensureNoPortalNotification(win) {
  let notificationBox =
    win.document.getElementById("high-priority-global-notificationbox");
  is(notificationBox.getNotificationWithValue(PORTAL_NOTIFICATION_VALUE), null,
    "There should be no captive portal notification in the window.");
}

/**
 * Some tests open a new window and close it later. When the window is closed,
 * the original window opened by mochitest gains focus, generating a
 * xul-window-visible notification. If the next test also opens a new window
 * before this notification has a chance to fire, CaptivePortalWatcher picks
 * up the first one instead of the one from the new window. To avoid this
 * unfortunate intermittent timing issue, we wait for the notification from
 * the original window every time we close a window that we opened.
 */
function waitForXulWindowVisible() {
  return new Promise(resolve => {
    Services.obs.addObserver(function observe() {
      Services.obs.removeObserver(observe, "xul-window-visible");
      resolve();
    }, "xul-window-visible");
  });
}

async function closeWindowAndWaitForXulWindowVisible(win) {
  let p = waitForXulWindowVisible();
  await BrowserTestUtils.closeWindow(win);
  await p;
}

/**
 * BrowserTestUtils.openNewBrowserWindow() does not guarantee the newly
 * opened window has received focus when the promise resolves, so we
 * have to manually wait every time.
 */
async function openWindowAndWaitForFocus() {
  let win = await BrowserTestUtils.openNewBrowserWindow();
  await SimpleTest.promiseFocus(win);
  return win;
}
