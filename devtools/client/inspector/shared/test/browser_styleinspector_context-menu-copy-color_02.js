/* vim: set ft=javascript ts=2 et sw=2 tw=80: */
/* Any copyright is dedicated to the Public Domain.
 http://creativecommons.org/publicdomain/zero/1.0/ */
"use strict";

// Test "Copy color" item of the context menu #2: Test that correct color is
// copied if the color changes.

const TEST_URI = `
  <style type="text/css">
    div {
      color: #123ABC;
    }
  </style>
  <div>Testing the color picker tooltip!</div>
`;

add_task(async function() {
  await addTab("data:text/html;charset=utf-8," + encodeURIComponent(TEST_URI));

  let {inspector, view} = await openRuleView();

  await testCopyToClipboard(inspector, view);
  await testManualEdit(inspector, view);
  await testColorPickerEdit(inspector, view);
});

async function testCopyToClipboard(inspector, view) {
  info("Testing that color is copied to clipboard");

  await selectNode("div", inspector);

  let element = getRuleViewProperty(view, "div", "color").valueSpan
    .querySelector(".ruleview-colorswatch");

  let allMenuItems = openStyleContextMenuAndGetAllItems(view, element);
  let menuitemCopyColor = allMenuItems.find(item => item.label ===
    STYLE_INSPECTOR_L10N.getStr("styleinspector.contextmenu.copyColor"));

  ok(menuitemCopyColor.visible, "Copy color is visible");

  await waitForClipboardPromise(() => menuitemCopyColor.click(),
    "#123ABC");

  EventUtils.synthesizeKey("KEY_Escape");
}

async function testManualEdit(inspector, view) {
  info("Testing manually edited colors");
  await selectNode("div", inspector);

  let {valueSpan} = getRuleViewProperty(view, "div", "color");

  let newColor = "#C9184E";
  let editor = await focusEditableField(view, valueSpan);

  info("Typing new value");
  let input = editor.input;
  let onBlur = once(input, "blur");
  EventUtils.sendString(newColor + ";", view.styleWindow);
  await onBlur;
  await wait(1);

  let colorValueElement = getRuleViewProperty(view, "div", "color")
    .valueSpan.firstChild;
  is(colorValueElement.dataset.color, newColor, "data-color was updated");

  view.styleDocument.popupNode = colorValueElement;

  let contextMenu = view.contextMenu;
  contextMenu._isColorPopup();
  is(contextMenu._colorToCopy, newColor, "_colorToCopy has the new value");
}

async function testColorPickerEdit(inspector, view) {
  info("Testing colors edited via color picker");
  await selectNode("div", inspector);

  let swatchElement = getRuleViewProperty(view, "div", "color").valueSpan
    .querySelector(".ruleview-colorswatch");

  info("Opening the color picker");
  let picker = view.tooltips.getTooltip("colorPicker");
  let onColorPickerReady = picker.once("ready");
  swatchElement.click();
  await onColorPickerReady;

  let rgbaColor = [83, 183, 89, 1];
  let rgbaColorText = "rgba(83, 183, 89, 1)";
  await simulateColorPickerChange(view, picker, rgbaColor);

  is(swatchElement.parentNode.dataset.color, rgbaColorText,
    "data-color was updated");
  view.styleDocument.popupNode = swatchElement;

  let contextMenu = view.contextMenu;
  contextMenu._isColorPopup();
  is(contextMenu._colorToCopy, rgbaColorText, "_colorToCopy has the new value");
}
