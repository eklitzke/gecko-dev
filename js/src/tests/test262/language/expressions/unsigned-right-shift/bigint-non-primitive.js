// |reftest| skip-if(!this.hasOwnProperty('BigInt')) -- BigInt is not enabled unconditionally
// Copyright (C) 2017 Josh Wolfe. All rights reserved.
// This code is governed by the BSD license found in the LICENSE file.

/*---
description: Unsigned right shift always throws for non-primitive BigInt values
esid: sec-unsigned-right-shift-operator-runtime-semantics-evaluation
info: |
  ShiftExpression : ShiftExpression >>> AdditiveExpression

  1. Let lref be the result of evaluating ShiftExpression.
  2. Let lval be ? GetValue(lref).
  3. Let rref be the result of evaluating AdditiveExpression.
  4. Let rval be ? GetValue(rref).
  5. Let lnum be ? ToNumeric(lval).
  6. Let rnum be ? ToNumeric(rval).
  7. If Type(lnum) does not equal Type(rnum), throw a TypeError exception.
  8. Let T be Type(lnum).
  9. Return T::unsignedRightShift(lnum, rnum).

  Note: BigInt::unsignedRightShift always throws a TypeError

features: [BigInt, Symbol.toPrimitive]
---*/

assert.throws(TypeError,
  function() { Object(0b101n) >>> 1n; },
  "bigint >>> bigint throws a TypeError for Object(0b101n) >>> 1n");
assert.throws(TypeError,
  function() { Object(0b101n) >>> Object(1n); },
  "bigint >>> bigint throws a TypeError for Object(0b101n) >>> Object(1n)");

function err() {
  throw new Test262Error();
}

assert.throws(TypeError,
  function() { ({[Symbol.toPrimitive]: function() { return 0b101n; }, valueOf: err, toString: err} >>> 1n); },
  "bigint >>> bigint throws a TypeError for primitive from @@toPrimitive");
assert.throws(TypeError,
  function() { ({valueOf: function() { return 0b101n; }, toString: err} >>> 1n); },
  "bigint >>> bigint throws a TypeError for primitive from {}.valueOf");
assert.throws(TypeError,
  function() { ({toString: function() { return 0b101n; }} >>> 1n); },
  "bigint >>> bigint throws a TypeError for primitive from {}.toString");
assert.throws(TypeError,
  function() { 0b101n >>> {[Symbol.toPrimitive]: function() { return 1n; }, valueOf: err, toString: err}; },
  "bigint >>> bigint throws a TypeError for primitive from @@toPrimitive");
assert.throws(TypeError,
  function() { 0b101n >>> {valueOf: function() { return 1n; }, toString: err}; },
  "bigint >>> bigint throws a TypeError for primitive from {}.valueOf");
assert.throws(TypeError,
  function() { 0b101n >>> {toString: function() { return 1n; }}; },
  "bigint >>> bigint throws a TypeError for primitive from {}.toString");
assert.throws(TypeError,
  function() { ({valueOf: function() { return 0b101n; }} >>> {valueOf: function() { return 1n; }}); },
  "bigint >>> bigint throws a TypeError for primitive from {}.valueOf");

reportCompare(0, 0);
