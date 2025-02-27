/* vim: set ft=javascript ts=2 et sw=2 tw=80: */
/* Any copyright is dedicated to the Public Domain.
 http://creativecommons.org/publicdomain/zero/1.0/ */

"use strict";

// Tests that the shapes highlighter is hidden when the highlighted shape container is
// removed from the page.

const TEST_URI = `
  <style type='text/css'>
    #shape {
      width: 800px;
      height: 800px;
      clip-path: circle(25%);
    }
  </style>
  <div id="shape"></div>
`;

add_task(async function() {
  await addTab("data:text/html;charset=utf-8," + encodeURIComponent(TEST_URI));
  let {inspector, view, testActor} = await openRuleView();
  let highlighters = view.highlighters;

  info("Select a node with a shape value");
  await selectNode("#shape", inspector);
  let container = getRuleViewProperty(view, "#shape", "clip-path").valueSpan;
  let shapeToggle = container.querySelector(".ruleview-shapeswatch");

  info("Toggling ON the CSS shapes highlighter from the rule-view.");
  let onHighlighterShown = highlighters.once("shapes-highlighter-shown");
  shapeToggle.click();
  await onHighlighterShown;
  ok(highlighters.shapesHighlighterShown, "CSS shapes highlighter is shown.");

  let onHighlighterHidden = highlighters.once("shapes-highlighter-hidden");
  info("Remove the #shapes container in the content page");
  testActor.eval(`
    document.querySelector("#shape").remove();
  `);
  await onHighlighterHidden;
  ok(!highlighters.shapesHighlighterShown, "CSS shapes highlighter is hidden.");
});
