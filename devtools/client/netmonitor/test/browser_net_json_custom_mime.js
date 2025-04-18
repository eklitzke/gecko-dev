/* Any copyright is dedicated to the Public Domain.
   http://creativecommons.org/publicdomain/zero/1.0/ */

"use strict";

/**
 * Tests if JSON responses with unusal/custom MIME types are handled correctly.
 */

add_task(async function() {
  let { tab, monitor } = await initNetMonitor(JSON_CUSTOM_MIME_URL);
  info("Starting test... ");

  let { document, store, windowRequire } = monitor.panelWin;
  let Actions = windowRequire("devtools/client/netmonitor/src/actions/index");
  let { L10N } = windowRequire("devtools/client/netmonitor/src/utils/l10n");
  let {
    getDisplayedRequests,
    getSortedRequests,
  } = windowRequire("devtools/client/netmonitor/src/selectors/index");

  store.dispatch(Actions.batchEnable(false));

  // Execute requests.
  await performRequests(monitor, tab, 1);

  let requestItem = document.querySelector(".request-list-item");
  let requestsListStatus = requestItem.querySelector(".status-code");
  EventUtils.sendMouseEvent({ type: "mouseover" }, requestsListStatus);
  await waitUntil(() => requestsListStatus.title);

  verifyRequestItemTarget(
    document,
    getDisplayedRequests(store.getState()),
    getSortedRequests(store.getState()).get(0),
    "GET",
    CONTENT_TYPE_SJS + "?fmt=json-custom-mime",
    {
      status: 200,
      statusText: "OK",
      type: "x-bigcorp-json",
      fullMimeType: "text/x-bigcorp-json; charset=utf-8",
      size: L10N.getFormatStrWithNumbers("networkMenu.sizeB", 41),
      time: true
    });

  wait = waitForDOM(document, "#response-panel .CodeMirror-code");
  store.dispatch(Actions.toggleNetworkDetails());
  EventUtils.sendMouseEvent({ type: "click" },
    document.querySelector("#response-tab"));
  await wait;

  testResponseTab();

  await teardown(monitor);

  function testResponseTab() {
    let tabpanel = document.querySelector("#response-panel");

    is(tabpanel.querySelector(".response-error-header") === null, true,
      "The response error header doesn't have the intended visibility.");
    let jsonView = tabpanel.querySelector(".tree-section .treeLabel") || {};
    is(jsonView.textContent === L10N.getStr("jsonScopeName"), true,
      "The response json view has the intended visibility.");
    is(tabpanel.querySelector(".CodeMirror-code") === null, false,
      "The response editor has the intended visibility.");
    is(tabpanel.querySelector(".response-image-box") === null, true,
      "The response image box doesn't have the intended visibility.");

    is(tabpanel.querySelectorAll(".tree-section").length, 2,
      "There should be 2 tree sections displayed in this tabpanel.");
    is(tabpanel.querySelectorAll(".treeRow:not(.tree-section)").length, 1,
      "There should be 1 json properties displayed in this tabpanel.");
    is(tabpanel.querySelectorAll(".empty-notice").length, 0,
      "The empty notice should not be displayed in this tabpanel.");

    let labels = tabpanel
      .querySelectorAll("tr:not(.tree-section) .treeLabelCell .treeLabel");
    let values = tabpanel
      .querySelectorAll("tr:not(.tree-section) .treeValueCell .objectBox");

    is(labels[0].textContent, "greeting",
      "The first json property name was incorrect.");
    is(values[0].textContent, "Hello oddly-named JSON!",
      "The first json property value was incorrect.");
  }
});
