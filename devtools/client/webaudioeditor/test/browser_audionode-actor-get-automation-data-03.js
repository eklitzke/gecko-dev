/* Any copyright is dedicated to the Public Domain.
   http://creativecommons.org/publicdomain/zero/1.0/ */

/**
 * Test that `cancelScheduledEvents` clears out events on and after
 * its argument.
 */

add_task(async function() {
  let { target, front } = await initBackend(SIMPLE_CONTEXT_URL);
  let [_, [destNode, oscNode, gainNode]] = await Promise.all([
    front.setup({ reload: true }),
    get3(front, "create-node")
  ]);

  await oscNode.addAutomationEvent("frequency", "setValueAtTime", [300, 0]);
  await oscNode.addAutomationEvent("frequency", "linearRampToValueAtTime", [500, 0.9]);
  await oscNode.addAutomationEvent("frequency", "setValueAtTime", [700, 1]);
  await oscNode.addAutomationEvent("frequency", "exponentialRampToValueAtTime", [1000, 2]);
  await oscNode.addAutomationEvent("frequency", "cancelScheduledValues", [1]);

  var { events, values } = await oscNode.getAutomationData("frequency");

  is(events.length, 2, "2 recorded events returned.");
  is(values.length, 2000, "2000 value points returned");

  checkAutomationValue(values, 0, 300);
  checkAutomationValue(values, 0.5, 411.15);
  checkAutomationValue(values, 0.9, 499.9);
  checkAutomationValue(values, 1, 499.9);
  checkAutomationValue(values, 2, 499.9);

  await removeTab(target.tab);
});
