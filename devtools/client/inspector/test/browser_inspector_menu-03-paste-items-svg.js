/* vim: set ts=2 et sw=2 tw=80: */
/* Any copyright is dedicated to the Public Domain.
http://creativecommons.org/publicdomain/zero/1.0/ */
"use strict";

// Test that HTML can be pasted in SVG elements.

const TEST_URL = URL_ROOT + "doc_inspector_svg.svg";
const PASTE_AS_FIRST_CHILD = '<circle xmlns="http://www.w3.org/2000/svg" cx="42" cy="42" r="5"/>';
const PASTE_AS_LAST_CHILD = '<circle xmlns="http://www.w3.org/2000/svg" cx="42" cy="42" r="15"/>';

add_task(async function() {
  let clipboard = require("devtools/shared/platform/clipboard");

  let { inspector, testActor } = await openInspectorForURL(TEST_URL);

  let refSelector = "svg";
  let oldHTML = await testActor.getProperty(refSelector, "innerHTML");
  await selectNode(refSelector, inspector);
  let markupTagLine = getContainerForSelector(refSelector, inspector).tagLine;

  await pasteContent("node-menu-pastefirstchild", PASTE_AS_FIRST_CHILD);
  await pasteContent("node-menu-pastelastchild", PASTE_AS_LAST_CHILD);

  let html = await testActor.getProperty(refSelector, "innerHTML");
  let expectedHtml = PASTE_AS_FIRST_CHILD + oldHTML + PASTE_AS_LAST_CHILD;
  is(html, expectedHtml, "The innerHTML of the SVG node is correct");

  // Helpers
  async function pasteContent(menuId, clipboardData) {
    let allMenuItems = openContextMenuAndGetAllItems(inspector, {
      target: markupTagLine,
    });
    info(`Testing ${menuId} for ${clipboardData}`);

    await SimpleTest.promiseClipboardChange(clipboardData,
      () => {
        clipboard.copyString(clipboardData);
      }
    );

    let onMutation = inspector.once("markupmutation");
    allMenuItems.find(item => item.id === menuId).click();
    info("Waiting for mutation to occur");
    await onMutation;
  }
});
