/* Any copyright is dedicated to the Public Domain.
   http://creativecommons.org/publicdomain/zero/1.0/ */

"use strict";

// Tests if a selection is dropped when clicking outside of it.

const TEST_DATA = [
  { delta: 112, value: 48 }, { delta: 213, value: 59 },
  { delta: 313, value: 60 }, { delta: 413, value: 59 },
  { delta: 530, value: 59 }, { delta: 646, value: 58 },
  { delta: 747, value: 60 }, { delta: 863, value: 48 },
  { delta: 980, value: 37 }, { delta: 1097, value: 30 },
  { delta: 1213, value: 29 }, { delta: 1330, value: 23 },
  { delta: 1430, value: 10 }, { delta: 1534, value: 17 },
  { delta: 1645, value: 20 }, { delta: 1746, value: 22 },
  { delta: 1846, value: 39 }, { delta: 1963, value: 26 },
  { delta: 2080, value: 27 }, { delta: 2197, value: 35 },
  { delta: 2312, value: 47 }, { delta: 2412, value: 53 },
  { delta: 2514, value: 60 }, { delta: 2630, value: 37 },
  { delta: 2730, value: 36 }, { delta: 2830, value: 37 },
  { delta: 2946, value: 36 }, { delta: 3046, value: 40 },
  { delta: 3163, value: 47 }, { delta: 3280, value: 41 },
  { delta: 3380, value: 35 }, { delta: 3480, value: 27 },
  { delta: 3580, value: 39 }, { delta: 3680, value: 42 },
  { delta: 3780, value: 49 }, { delta: 3880, value: 55 },
  { delta: 3980, value: 60 }, { delta: 4080, value: 60 },
  { delta: 4180, value: 60 }
];
const LineGraphWidget = require("devtools/client/shared/widgets/LineGraphWidget");

add_task(async function() {
  await addTab("about:blank");
  await performTest();
  gBrowser.removeCurrentTab();
});

async function performTest() {
  let [host,, doc] = await createHost();
  let graph = new LineGraphWidget(doc.body, "fps");
  await graph.once("ready");

  testGraph(graph);

  await graph.destroy();
  host.destroy();
}

function testGraph(graph) {
  graph.setData(TEST_DATA);

  dragStart(graph, 300);
  dragStop(graph, 500);
  ok(graph.hasSelection(),
    "A selection should be available.");
  is(graph.getSelection().start, 300,
    "The current selection start value is correct.");
  is(graph.getSelection().end, 500,
    "The current selection end value is correct.");

  click(graph, 600);
  ok(!graph.hasSelection(),
    "The selection should be dropped.");
}

// EventUtils just doesn't work!

function click(graph, x, y = 1) {
  x /= window.devicePixelRatio;
  y /= window.devicePixelRatio;
  graph._onMouseMove({ testX: x, testY: y });
  graph._onMouseDown({ testX: x, testY: y });
  graph._onMouseUp({ testX: x, testY: y });
}

function dragStart(graph, x, y = 1) {
  x /= window.devicePixelRatio;
  y /= window.devicePixelRatio;
  graph._onMouseMove({ testX: x, testY: y });
  graph._onMouseDown({ testX: x, testY: y });
}

function dragStop(graph, x, y = 1) {
  x /= window.devicePixelRatio;
  y /= window.devicePixelRatio;
  graph._onMouseMove({ testX: x, testY: y });
  graph._onMouseUp({ testX: x, testY: y });
}
