"use strict";

// This function is pretty tightly tied to Extension.jsm.
// Its job is to fill in the |tab| property of the sender.
const getSender = (extension, target, sender) => {
  let tabId = -1;
  if ("tabId" in sender) {
    // The message came from a privileged extension page running in a tab. In
    // that case, it should include a tabId property (which is filled in by the
    // page-open listener below).
    tabId = sender.tabId;
    delete sender.tabId;
  } else if (ChromeUtils.getClassName(target) == "XULElement") {
    tabId = tabTracker.getBrowserData(target).tabId;
  }

  if (tabId != null && tabId >= 0) {
    let tab = extension.tabManager.get(tabId, null);
    if (tab) {
      sender.tab = tab.convert();
    }
  }
};

// Used by Extension.jsm
global.tabGetSender = getSender;

/* eslint-disable mozilla/balanced-listeners */
extensions.on("page-shutdown", (type, context) => {
  if (context.viewType == "tab") {
    if (context.extension.id !== context.xulBrowser.contentPrincipal.addonId) {
      // Only close extension tabs.
      // This check prevents about:addons from closing when it contains a
      // WebExtension as an embedded inline options page.
      return;
    }
    let {BrowserApp} = context.xulBrowser.ownerGlobal;
    if (BrowserApp) {
      let nativeTab = BrowserApp.getTabForBrowser(context.xulBrowser);
      if (nativeTab) {
        BrowserApp.closeTab(nativeTab);
      }
    }
  }
});
/* eslint-enable mozilla/balanced-listeners */

global.openOptionsPage = (extension) => {
  let window = windowTracker.topWindow;
  if (!window) {
    return Promise.reject({message: "No browser window available"});
  }

  let {BrowserApp} = window;

  if (extension.manifest.options_ui.open_in_tab) {
    BrowserApp.selectOrAddTab(extension.manifest.options_ui.page, {
      selected: true,
      parentId: BrowserApp.selectedTab.id,
    });
  } else {
    BrowserApp.openAddonManager({addonId: extension.id});
  }

  return Promise.resolve();
};

extensions.registerModules({
  browserAction: {
    url: "chrome://browser/content/ext-browserAction.js",
    schema: "chrome://browser/content/schemas/browser_action.json",
    scopes: ["addon_parent"],
    manifest: ["browser_action"],
    paths: [
      ["browserAction"],
    ],
  },
  browsingData: {
    url: "chrome://browser/content/ext-browsingData.js",
    schema: "chrome://browser/content/schemas/browsing_data.json",
    scopes: ["addon_parent"],
    manifest: ["browsing_data"],
    paths: [
      ["browsingData"],
    ],
  },
  pageAction: {
    url: "chrome://browser/content/ext-pageAction.js",
    schema: "chrome://browser/content/schemas/page_action.json",
    scopes: ["addon_parent"],
    manifest: ["page_action"],
    paths: [
      ["pageAction"],
    ],
  },
  tabs: {
    url: "chrome://browser/content/ext-tabs.js",
    schema: "chrome://browser/content/schemas/tabs.json",
    scopes: ["addon_parent"],
    paths: [
      ["tabs"],
    ],
  },
});

