// |reftest| skip-if(!this.hasOwnProperty('Atomics')||!this.hasOwnProperty('SharedArrayBuffer')) -- Atomics,SharedArrayBuffer is not enabled unconditionally
// Copyright (C) 2018 Amal Hussein. All rights reserved.
// This code is governed by the BSD license found in the LICENSE file.

/*---
esid: sec-atomics.wait
description: >
  Throws a RangeError is index < 0
info: |
  Atomics.wait( typedArray, index, value, timeout )

  2.Let i be ? ValidateAtomicAccess(typedArray, index).
    ...
      2.Let accessIndex be ? ToIndex(requestIndex).
        ...
        2.b If integerIndex < 0, throw a RangeError exception
features: [ Atomics , SharedArrayBuffer, TypedArray ]
---*/

var sab = new SharedArrayBuffer(1024);
var int32Array = new Int32Array(sab);
var poisoned = {
  valueOf: function() {
    throw new Test262Error("should not evaluate this code");
  }
};

assert.throws(RangeError, () => Atomics.wait(int32Array, -Infinity, poisoned, poisoned));
assert.throws(RangeError, () => Atomics.wait(int32Array, -7.999, poisoned, poisoned));
assert.throws(RangeError, () => Atomics.wait(int32Array, -1, poisoned, poisoned));
assert.throws(RangeError, () => Atomics.wait(int32Array, -300, poisoned, poisoned));

reportCompare(0, 0);
