/* Any copyright is dedicated to the Public Domain.
   http://creativecommons.org/publicdomain/zero/1.0/ */

"use strict";

// Tests deleting presets

const {CSSFilterEditorWidget} = require("devtools/client/shared/widgets/FilterWidget");
const {getClientCssProperties} = require("devtools/shared/fronts/css-properties");

const TEST_URI = CHROME_URL_ROOT + "doc_filter-editor-01.html";

add_task(async function() {
  let [,, doc] = await createHost("bottom", TEST_URI);
  const cssIsValid = getClientCssProperties().getValidityChecker(doc);

  const container = doc.querySelector("#filter-container");
  let widget = new CSSFilterEditorWidget(container, "none", cssIsValid);
  // First render
  await widget.once("render");

  const NAME = "Test";
  const VALUE = "blur(2px) contrast(150%)";

  await showFilterPopupPresetsAndCreatePreset(widget, NAME, VALUE);

  let removeButton = widget.el.querySelector(".preset .remove-button");
  let onRender = widget.once("render");
  widget._presetClick({
    target: removeButton
  });

  await onRender;
  is(widget.el.querySelector(".preset"), null,
     "Should re-render after removing preset");

  let list = await widget.getPresets();
  is(list.length, 0,
     "Should remove presets from asyncStorage");
});
