// |reftest| skip-if(!this.hasOwnProperty('BigInt')) -- BigInt is not enabled unconditionally
// Copyright (C) 2017 Josh Wolfe. All rights reserved.
// This code is governed by the BSD license found in the LICENSE file.
/*---
description: bitwise-or operator ToNumeric with BigInt operands
esid: sec-binary-bitwise-operators-runtime-semantics-evaluation
features: [BigInt, Symbol.toPrimitive, computed-property-names]
---*/

assert.sameValue(Object(2n) | 1n, 3n, "ToPrimitive: unbox object with internal slot");
assert.sameValue(1n | Object(2n), 3n, "ToPrimitive: unbox object with internal slot");
assert.sameValue(({
  [Symbol.toPrimitive]: function() {
    return 2n;
  }
}) | 1n, 3n, "ToPrimitive: @@toPrimitive");
assert.sameValue(1n | {
  [Symbol.toPrimitive]: function() {
    return 2n;
  }
}, 3n, "ToPrimitive: @@toPrimitive");
assert.sameValue(({
  valueOf: function() {
    return 2n;
  }
}) | 1n, 3n, "ToPrimitive: valueOf");
assert.sameValue(1n | {
  valueOf: function() {
    return 2n;
  }
}, 3n, "ToPrimitive: valueOf");
assert.sameValue(({
  toString: function() {
    return 2n;
  }
}) | 1n, 3n, "ToPrimitive: toString");
assert.sameValue(1n | {
  toString: function() {
    return 2n;
  }
}, 3n, "ToPrimitive: toString");

reportCompare(0, 0);
