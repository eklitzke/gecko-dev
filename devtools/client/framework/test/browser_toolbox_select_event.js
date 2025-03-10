/* -*- indent-tabs-mode: nil; js-indent-level: 2 -*- */
/* vim: set ft=javascript ts=2 et sw=2 tw=80: */
/* Any copyright is dedicated to the Public Domain.
 * http://creativecommons.org/publicdomain/zero/1.0/ */

"use strict";

const PAGE_URL = "data:text/html;charset=utf-8,test select events";

requestLongerTimeout(2);

add_task(async function() {
  let tab = await addTab(PAGE_URL);

  let toolbox = await openToolboxForTab(tab, "webconsole", "bottom");
  await testSelectEvent("inspector");
  await testSelectEvent("webconsole");
  await testSelectEvent("styleeditor");
  await testSelectEvent("inspector");
  await testSelectEvent("webconsole");
  await testSelectEvent("styleeditor");

  await testToolSelectEvent("inspector");
  await testToolSelectEvent("webconsole");
  await testToolSelectEvent("styleeditor");
  await toolbox.destroy();

  toolbox = await openToolboxForTab(tab, "webconsole", "side");
  await testSelectEvent("inspector");
  await testSelectEvent("webconsole");
  await testSelectEvent("styleeditor");
  await testSelectEvent("inspector");
  await testSelectEvent("webconsole");
  await testSelectEvent("styleeditor");
  await toolbox.destroy();

  toolbox = await openToolboxForTab(tab, "webconsole", "window");
  await testSelectEvent("inspector");
  await testSelectEvent("webconsole");
  await testSelectEvent("styleeditor");
  await testSelectEvent("inspector");
  await testSelectEvent("webconsole");
  await testSelectEvent("styleeditor");
  await toolbox.destroy();

  await testSelectToolRace();

  /**
   * Assert that selecting the given toolId raises a select event
   * @param {toolId} Id of the tool to test
   */
  async function testSelectEvent(toolId) {
    let onSelect = toolbox.once("select");
    toolbox.selectTool(toolId);
    let id = await onSelect;
    is(id, toolId, toolId + " selected");
  }

  /**
   * Assert that selecting the given toolId raises its corresponding
   * selected event
   * @param {toolId} Id of the tool to test
   */
  async function testToolSelectEvent(toolId) {
    let onSelected = toolbox.once(toolId + "-selected");
    toolbox.selectTool(toolId);
    await onSelected;
    is(toolbox.currentToolId, toolId, toolId + " tool selected");
  }

  /**
   * Assert that two calls to selectTool won't race
   */
  async function testSelectToolRace() {
    let toolbox = await openToolboxForTab(tab, "webconsole");
    let selected = false;
    let onSelect = (event, id) => {
      if (selected) {
        ok(false, "Got more than one 'select' event");
      } else {
        selected = true;
      }
    };
    toolbox.once("select", onSelect);
    let p1 = toolbox.selectTool("inspector");
    let p2 = toolbox.selectTool("inspector");
    // Check that both promises don't resolve too early
    let checkSelectToolResolution = panel => {
      ok(selected, "selectTool resolves only after 'select' event is fired");
      let inspector = toolbox.getPanel("inspector");
      is(panel, inspector, "selecTool resolves to the panel instance");
    };
    p1.then(checkSelectToolResolution);
    p2.then(checkSelectToolResolution);
    await p1;
    await p2;

    await toolbox.destroy();
  }
});

