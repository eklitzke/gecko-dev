/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

"use strict";

// Test that shapes are updated correctly for scaling on one axis in transform mode.

const TEST_URL = URL_ROOT + "doc_inspector_highlighter_cssshapes.html";
const HIGHLIGHTER_TYPE = "ShapesHighlighter";
const SHAPE_SELECTORS = ["#polygon-transform", "#ellipse"];

add_task(async function() {
  let env = await openInspectorForURL(TEST_URL);
  let helper = await getHighlighterHelperFor(HIGHLIGHTER_TYPE)(env);
  let {testActor, inspector} = env;
  let view = selectRuleView(inspector);
  let highlighters = view.highlighters;
  let config = { inspector, view, highlighters, testActor, helper };

  await testOneDimScale(config);
});

async function setup(config) {
  const { inspector, view, selector, property, options } = config;
  await selectNode(selector, inspector);
  await toggleShapesHighlighter(view, selector, property, true, options);
}

async function teardown(config) {
  const { view, selector, property } = config;
  info(`Turn off shapes highlighter for ${selector}`);
  await toggleShapesHighlighter(view, selector, property, false);
}

async function testOneDimScale(config) {
  const { testActor, helper, highlighters } = config;
  const options = { transformMode: true };
  const property = "clip-path";

  for (let selector of SHAPE_SELECTORS) {
    await setup({selector, property, options, ...config});
    let { mouse } = helper;

    let { nw, width,
          height, center } = await getBoundingBoxInPx({selector, ...config});

    // if the top or left edges are not visible, move the shape so it is.
    if (nw[0] < 0 || nw[1] < 0) {
      let [x, y] = center;
      let dx = Math.max(0, -nw[0]);
      let dy = Math.max(0, -nw[1]);
      await mouse.down(x, y, selector);
      await mouse.move(x + dx, y + dy, selector);
      await mouse.up(x + dx, y + dy, selector);
      await testActor.reflow();
      nw[0] += dx;
      nw[1] += dy;
    }
    let dx = width / 10;
    let dy = height / 10;
    let onShapeChangeApplied;

    info("Scaling from w");
    onShapeChangeApplied = highlighters.once("shapes-highlighter-changes-applied");
    await mouse.down(nw[0], center[1], selector);
    await mouse.move(nw[0] + dx, center[1], selector);
    await mouse.up(nw[0] + dx, center[1], selector);
    await testActor.reflow();
    await onShapeChangeApplied;

    let wBB = await getBoundingBoxInPx({selector, ...config});
    isnot(wBB.nw[0], nw[0], `${selector} nw moved right after w scale`);
    is(wBB.nw[1], nw[1], `${selector} nw not moved down after w scale`);
    isnot(wBB.width, width, `${selector} width reduced after w scale`);
    is(wBB.height, height, `${selector} height not reduced after w scale`);

    info("Scaling from e");
    onShapeChangeApplied = highlighters.once("shapes-highlighter-changes-applied");
    await mouse.down(wBB.ne[0], center[1], selector);
    await mouse.move(wBB.ne[0] - dx, center[1], selector);
    await mouse.up(wBB.ne[0] - dx, center[1], selector);
    await testActor.reflow();
    await onShapeChangeApplied;

    let eBB = await getBoundingBoxInPx({selector, ...config});
    isnot(eBB.ne[0], wBB.ne[0], `${selector} ne moved left after e scale`);
    is(eBB.ne[1], wBB.ne[1], `${selector} ne not moved down after e scale`);
    isnot(eBB.width, wBB.width, `${selector} width reduced after e scale`);
    is(eBB.height, wBB.height, `${selector} height not reduced after e scale`);

    info("Scaling from s");
    onShapeChangeApplied = highlighters.once("shapes-highlighter-changes-applied");
    await mouse.down(eBB.center[0], eBB.sw[1], selector);
    await mouse.move(eBB.center[0], eBB.sw[1] - dy, selector);
    await mouse.up(eBB.center[0], eBB.sw[1] - dy, selector);
    await testActor.reflow();
    await onShapeChangeApplied;

    let sBB = await getBoundingBoxInPx({selector, ...config});
    is(sBB.sw[0], eBB.sw[0], `${selector} sw not moved right after w scale`);
    isnot(sBB.sw[1], eBB.sw[1], `${selector} sw moved down after w scale`);
    is(sBB.width, eBB.width, `${selector} width not reduced after w scale`);
    isnot(sBB.height, eBB.height, `${selector} height reduced after w scale`);

    info("Scaling from n");
    onShapeChangeApplied = highlighters.once("shapes-highlighter-changes-applied");
    await mouse.down(sBB.center[0], sBB.nw[1], selector);
    await mouse.move(sBB.center[0], sBB.nw[1] + dy, selector);
    await mouse.up(sBB.center[0], sBB.nw[1] + dy, selector);
    await testActor.reflow();
    await onShapeChangeApplied;

    let nBB = await getBoundingBoxInPx({selector, ...config});
    is(nBB.nw[0], sBB.nw[0], `${selector} nw not moved right after n scale`);
    isnot(nBB.nw[1], sBB.nw[1], `${selector} nw moved down after n scale`);
    is(nBB.width, sBB.width, `${selector} width reduced after n scale`);
    isnot(nBB.height, sBB.height, `${selector} height not reduced after n scale`);

    await teardown({selector, property, ...config});
  }
}

async function getBoundingBoxInPx(config) {
  const { testActor, selector, inspector, highlighters } = config;
  let quads = await testActor.getAllAdjustedQuads(selector);
  let { width, height } = quads.content[0].bounds;
  let highlightedNode = await getNodeFront(selector, inspector);
  let computedStyle = await inspector.pageStyle.getComputed(highlightedNode);
  let paddingTop = parseFloat(computedStyle["padding-top"].value);
  let paddingLeft = parseFloat(computedStyle["padding-left"].value);
  // path is always of form "Mx y Lx y Lx y Lx y Z", where x/y are numbers
  let path = await testActor.getHighlighterNodeAttribute(
    "shapes-bounding-box", "d", highlighters.highlighters[HIGHLIGHTER_TYPE]);
  let coords = path.replace(/[MLZ]/g, "").split(" ").map((n, i) => {
    return i % 2 === 0 ? paddingLeft + width * n / 100 : paddingTop + height * n / 100;
  });

  let nw = [coords[0], coords[1]];
  let ne = [coords[2], coords[3]];
  let se = [coords[4], coords[5]];
  let sw = [coords[6], coords[7]];
  let center = [(nw[0] + se[0]) / 2, (nw[1] + se[1]) / 2];
  let shapeWidth = Math.sqrt((ne[0] - nw[0]) ** 2 + (ne[1] - nw[1]) ** 2);
  let shapeHeight = Math.sqrt((sw[0] - nw[0]) ** 2 + (sw[1] - nw[1]) ** 2);

  return { nw, ne, se, sw, center, width: shapeWidth, height: shapeHeight };
}
