/* -*- indent-tabs-mode: nil; js-indent-level: 2 -*- */
/* vim: set ft=javascript ts=2 et sw=2 tw=80: */
/* Any copyright is dedicated to the Public Domain.
 * http://creativecommons.org/publicdomain/zero/1.0/ */

"use strict";

// Test toggling the toolbox with ACCEL+SHIFT+I / ACCEL+ALT+I and F12 in docked
// and detached (window) modes.

const URL = "data:text/html;charset=utf-8,Toggling devtools using shortcuts";

var {Toolbox} = require("devtools/client/framework/toolbox");

add_task(async function() {
  // Make sure this test starts with the selectedTool pref cleared. Previous
  // tests select various tools, and that sets this pref.
  Services.prefs.clearUserPref("devtools.toolbox.selectedTool");

  // Test with ACCEL+SHIFT+I / ACCEL+ALT+I (MacOSX) ; modifiers should match :
  // - toolbox-key-toggle in devtools/client/framework/toolbox-window.xul
  // - key_devToolboxMenuItem in browser/base/content/browser.xul
  info("Test toggle using CTRL+SHIFT+I/CMD+ALT+I");
  await testToggle("I", {
    accelKey: true,
    shiftKey: !navigator.userAgent.match(/Mac/),
    altKey: navigator.userAgent.match(/Mac/)
  });

  // Test with F12 ; no modifiers
  info("Test toggle using F12");
  await testToggle("VK_F12", {});
});

async function testToggle(key, modifiers) {
  let tab = await addTab(URL + " ; key : '" + key + "'");
  await gDevTools.showToolbox(TargetFactory.forTab(tab));

  await testToggleDockedToolbox(tab, key, modifiers);
  await testToggleDetachedToolbox(tab, key, modifiers);

  await cleanup();
}

async function testToggleDockedToolbox(tab, key, modifiers) {
  let toolbox = getToolboxForTab(tab);

  isnot(toolbox.hostType, Toolbox.HostType.WINDOW,
    "Toolbox is docked in the main window");

  info("verify docked toolbox is destroyed when using toggle key");
  let onToolboxDestroyed = gDevTools.once("toolbox-destroyed");
  EventUtils.synthesizeKey(key, modifiers);
  await onToolboxDestroyed;
  ok(true, "Docked toolbox is destroyed when using a toggle key");

  info("verify new toolbox is created when using toggle key");
  let onToolboxReady = gDevTools.once("toolbox-ready");
  EventUtils.synthesizeKey(key, modifiers);
  await onToolboxReady;
  ok(true, "Toolbox is created by using when toggle key");
}

async function testToggleDetachedToolbox(tab, key, modifiers) {
  let toolbox = getToolboxForTab(tab);

  info("change the toolbox hostType to WINDOW");

  await toolbox.switchHost(Toolbox.HostType.WINDOW);
  is(toolbox.hostType, Toolbox.HostType.WINDOW,
    "Toolbox opened on separate window");

  info("Wait for focus on the toolbox window");
  await new Promise(res => waitForFocus(res, toolbox.win));

  info("Focus main window to put the toolbox window in the background");

  let onMainWindowFocus = once(window, "focus");
  window.focus();
  await onMainWindowFocus;
  ok(true, "Main window focused");

  info("Verify windowed toolbox is focused instead of closed when using " +
    "toggle key from the main window");
  let toolboxWindow = toolbox.win.top;
  let onToolboxWindowFocus = once(toolboxWindow, "focus", true);
  EventUtils.synthesizeKey(key, modifiers);
  await onToolboxWindowFocus;
  ok(true, "Toolbox focused and not destroyed");

  info("Verify windowed toolbox is destroyed when using toggle key from its " +
    "own window");

  let onToolboxDestroyed = gDevTools.once("toolbox-destroyed");
  EventUtils.synthesizeKey(key, modifiers, toolboxWindow);
  await onToolboxDestroyed;
  ok(true, "Toolbox destroyed");
}

function getToolboxForTab(tab) {
  return gDevTools.getToolbox(TargetFactory.forTab(tab));
}

function cleanup() {
  Services.prefs.setCharPref("devtools.toolbox.host", Toolbox.HostType.BOTTOM);
  gBrowser.removeCurrentTab();
}
