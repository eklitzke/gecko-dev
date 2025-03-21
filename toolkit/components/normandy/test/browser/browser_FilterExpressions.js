"use strict";

ChromeUtils.import("resource://normandy/lib/FilterExpressions.jsm", this);

// Basic JEXL tests
add_task(async function() {
  let val;
  // Test that basic expressions work
  val = await FilterExpressions.eval("2+2");
  is(val, 4, "basic expression works");

  // Test that multiline expressions work
  val = await FilterExpressions.eval(`
    2
    +
    2
  `);
  is(val, 4, "multiline expression works");

  // Test that it reads from the context correctly.
  val = await FilterExpressions.eval("first + second + 3", {first: 1, second: 2});
  is(val, 6, "context is available to filter expressions");
});

// Date tests
add_task(async function() {
  let val;
  // Test has a date transform
  val = await FilterExpressions.eval('"2016-04-22"|date');
  const d = new Date(Date.UTC(2016, 3, 22)); // months are 0 based
  is(val.toString(), d.toString(), "Date transform works");

  // Test dates are comparable
  const context = {someTime: Date.UTC(2016, 0, 1)};
  val = await FilterExpressions.eval('"2015-01-01"|date < someTime', context);
  ok(val, "dates are comparable with less-than");
  val = await FilterExpressions.eval('"2017-01-01"|date > someTime', context);
  ok(val, "dates are comparable with greater-than");
});

// Sampling tests
add_task(async function() {
  let val;
  // Test stable sample returns true for matching samples
  val = await FilterExpressions.eval('["test"]|stableSample(1)');
  ok(val, "Stable sample returns true for 100% sample");

  // Test stable sample returns true for matching samples
  val = await FilterExpressions.eval('["test"]|stableSample(0)');
  ok(!val, "Stable sample returns false for 0% sample");

  // Test stable sample for known samples
  val = await FilterExpressions.eval('["test-1"]|stableSample(0.5)');
  ok(val, "Stable sample returns true for a known sample");
  val = await FilterExpressions.eval('["test-4"]|stableSample(0.5)');
  ok(!val, "Stable sample returns false for a known sample");

  // Test bucket sample for known samples
  val = await FilterExpressions.eval('["test-1"]|bucketSample(0, 5, 10)');
  ok(val, "Bucket sample returns true for a known sample");
  val = await FilterExpressions.eval('["test-4"]|bucketSample(0, 5, 10)');
  ok(!val, "Bucket sample returns false for a known sample");
});

// Preference tests
add_task(async function() {
  let val;
  // Compare the value of the preference
  await SpecialPowers.pushPrefEnv({set: [["normandy.test.value", 3]]});
  val = await FilterExpressions.eval('"normandy.test.value"|preferenceValue == 3');
  ok(val, "preferenceValue expression compares against preference values");
  val = await FilterExpressions.eval('"normandy.test.value"|preferenceValue == "test"');
  ok(!val, "preferenceValue expression fails value checks appropriately");

  // preferenceValue can take a default value as an optional argument, which
  // defaults to `undefined`.
  val = await FilterExpressions.eval('"normandy.test.default"|preferenceValue(false) == false');
  ok(val, "preferenceValue takes optional 'default value' param for prefs without set values");
  val = await FilterExpressions.eval('"normandy.test.value"|preferenceValue(5) == 5');
  ok(!val, "preferenceValue default param is not returned for prefs with set values");

  // Compare if the preference is user set
  val = await FilterExpressions.eval('"normandy.test.isSet"|preferenceIsUserSet == true');
  ok(!val, "preferenceIsUserSet expression determines if preference is set at all");
  val = await FilterExpressions.eval('"normandy.test.value"|preferenceIsUserSet == true');
  ok(val, "preferenceIsUserSet expression determines if user's preference has been set");

  // Compare if the preference has _any_ value, whether it's user-set or default,
  val = await FilterExpressions.eval('"normandy.test.nonexistant"|preferenceExists == true');
  ok(!val, "preferenceExists expression determines if preference exists at all");
  val = await FilterExpressions.eval('"normandy.test.value"|preferenceExists == true');
  ok(val, "preferenceExists expression fails existence check appropriately");
});

// keys tests
add_task(async function testKeys() {
  let val;

  // Test an object defined in JEXL
  val = await FilterExpressions.eval("{foo: 1, bar: 2}|keys");
  Assert.deepEqual(
    new Set(val),
    new Set(["foo", "bar"]),
    "keys returns the keys from an object in JEXL",
  );

  // Test an object in the context
  let context = {ctxObject: {baz: "string", biff: NaN}};
  val = await FilterExpressions.eval("ctxObject|keys", context);

  Assert.deepEqual(
    new Set(val),
    new Set(["baz", "biff"]),
    "keys returns the keys from an object in the context",
  );

  // Test that values from the prototype are not included
  context = {ctxObject: Object.create({fooProto: 7})};
  context.ctxObject.baz = 8;
  context.ctxObject.biff = 5;
  is(
    await FilterExpressions.eval("ctxObject.fooProto", context),
    7,
    "Prototype properties are accessible via property access",
  );
  val = await FilterExpressions.eval("ctxObject|keys", context);
  Assert.deepEqual(
    new Set(val),
    new Set(["baz", "biff"]),
    "keys does not return properties from the object's prototype chain",
  );

  // Return undefined for non-objects
  is(
    await FilterExpressions.eval("ctxObject|keys", {ctxObject: 45}),
    undefined,
    "keys returns undefined for numbers",
  );
  is(
    await FilterExpressions.eval("ctxObject|keys", {ctxObject: null}),
    undefined,
    "keys returns undefined for null",
  );

  // Object properties are not cached
  let pong = 0;
  context = {ctxObject: {
    get ping() {
      return ++pong;
    }
  }};
  await FilterExpressions.eval("ctxObject.ping == 0 || ctxObject.ping == 1", context);
  is(pong, 2, "Properties are not reifed");
});

// intersect tests
add_task(async function testIntersect() {
  let val;

  val = await FilterExpressions.eval("[1, 2, 3] intersect [4, 2, 6, 7, 3]");
  Assert.deepEqual(
    new Set(val),
    new Set([2, 3]),
    "intersect finds the common elements between two lists in JEXL",
  );

  const context = {left: [5, 7], right: [4, 5, 3]};
  val = await FilterExpressions.eval("left intersect right", context);
  Assert.deepEqual(
    new Set(val),
    new Set([5]),
    "intersect finds the common elements between two lists in the context",
  );

  val = await FilterExpressions.eval("['string', 2] intersect [4, 'string', 'other', 3]");
  Assert.deepEqual(
    new Set(val),
    new Set(["string"]),
    "intersect can compare strings",
  );

  // Return undefined when intersecting things that aren't lists.
  is(
    await FilterExpressions.eval("5 intersect 7"),
    undefined,
    "intersect returns undefined for numbers",
  );
  is(
    await FilterExpressions.eval("val intersect other", {val: null, other: null}),
    undefined,
    "intersect returns undefined for null",
  );
  is(
    await FilterExpressions.eval("5 intersect [1, 2, 5]"),
    undefined,
    "intersect returns undefined if only one operand is a list",
  );
});
