/* -*- indent-tabs-mode: nil; js-indent-level: 2 -*- */
/* vim: set ts=2 et sw=2 tw=80: */
/* Any copyright is dedicated to the Public Domain.
 * http://creativecommons.org/publicdomain/zero/1.0/ */

"use strict";

const TEST_JSON_URL = URL_ROOT + "csp_json.json";

add_task(async function() {
  info("Test CSP JSON started");

  let tab = await addJsonViewTab(TEST_JSON_URL);

  let count = await getElementCount(".jsonPanelBox .treeTable .treeRow");
  is(count, 1, "There must be one row");

  // The JSON Viewer alters the CSP, but the displayed header should be the original one
  await selectJsonViewContentTab("headers");
  await ContentTask.spawn(tab.linkedBrowser, null, async function() {
    let responseHeaders = content.document.querySelector(".netHeadersGroup");
    let names = responseHeaders.querySelectorAll(".netInfoParamName");
    let found = false;
    for (let name of names) {
      if (name.textContent.toLowerCase() == "content-security-policy") {
        ok(!found, "The CSP header only appears once");
        found = true;
        let value = name.nextElementSibling.textContent;
        let expected = "default-src 'none'; base-uri 'none';";
        is(value, expected, "The CSP value has not been altered");
      }
    }
    ok(found, "The CSP header is present");
  });
});
