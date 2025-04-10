/* Any copyright is dedicated to the Public Domain.
   http://creativecommons.org/publicdomain/zero/1.0/ */

"use strict";

/**
 * Test that CORS preflight requests are displayed by network monitor
 */

add_task(async function() {
  let { tab, monitor } = await initNetMonitor(CORS_URL);

  let { document, store, windowRequire } = monitor.panelWin;
  let Actions = windowRequire("devtools/client/netmonitor/src/actions/index");
  let {
    getDisplayedRequests,
    getSortedRequests,
  } = windowRequire("devtools/client/netmonitor/src/selectors/index");

  store.dispatch(Actions.batchEnable(false));

  let wait = waitForNetworkEvents(monitor, 2);

  info("Performing a CORS request");
  let requestUrl = "http://test1.example.com" + CORS_SJS_PATH;
  await ContentTask.spawn(tab.linkedBrowser, requestUrl, async function(url) {
    content.wrappedJSObject.performRequests(url, "triggering/preflight", "post-data");
  });

  info("Waiting until the requests appear in netmonitor");
  await wait;

  info("Checking the preflight and flight methods");
  ["OPTIONS", "POST"].forEach((method, index) => {
    verifyRequestItemTarget(
      document,
      getDisplayedRequests(store.getState()),
      getSortedRequests(store.getState()).get(index),
      method,
      requestUrl
    );
  });

  await teardown(monitor);
});
