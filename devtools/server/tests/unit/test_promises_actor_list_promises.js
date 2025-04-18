/* Any copyright is dedicated to the Public Domain.
   http://creativecommons.org/publicdomain/zero/1.0/ */

/**
 * Test that we can get the list of all live Promise objects from the
 * PromisesActor.
 */

"use strict";

const { PromisesFront } = require("devtools/shared/fronts/promises");
const SECRET = "MyLittleSecret";

add_task(async function() {
  let client = await startTestDebuggerServer("promises-actor-test");
  let chromeActors = await getChromeActors(client);

  // We have to attach the chrome TabActor before playing with the PromiseActor
  await attachTab(client, chromeActors);
  await testListPromises(client, chromeActors, v =>
    new Promise(resolve => resolve(v)));

  let response = await listTabs(client);
  let targetTab = findTab(response.tabs, "promises-actor-test");
  ok(targetTab, "Found our target tab.");

  await testListPromises(client, targetTab, v => {
    const debuggee = DebuggerServer.getTestGlobal("promises-actor-test");
    return debuggee.Promise.resolve(v);
  });

  await close(client);
});

async function testListPromises(client, form, makePromise) {
  let resolution = SECRET + Math.random();
  let promise = makePromise(resolution);
  let front = PromisesFront(client, form);

  await front.attach();

  let promises = await front.listPromises();

  let found = false;
  for (let p of promises) {
    equal(p.type, "object", "Expect type to be Object");
    equal(p.class, "Promise", "Expect class to be Promise");
    equal(typeof p.promiseState.creationTimestamp, "number",
      "Expect creation timestamp to be a number");
    if (p.promiseState.state !== "pending") {
      equal(typeof p.promiseState.timeToSettle, "number",
        "Expect time to settle to be a number");
    }

    if (p.promiseState.state === "fulfilled" &&
        p.promiseState.value === resolution) {
      found = true;
    }
  }

  ok(found, "Found our promise");
  await front.detach();
  // Appease eslint
  void promise;
}
