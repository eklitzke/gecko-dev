// |reftest| skip-if(!this.hasOwnProperty('Atomics')) -- Atomics is not enabled unconditionally
// Copyright (C) 2017 Mozilla Corporation.  All rights reserved.
// This code is governed by the BSD license found in the LICENSE file.

/*---
esid: sec-atomics.or
description: >
  Test Atomics.or on shared non-integer TypedArrays
includes: [testTypedArray.js]
features: [Atomics, TypedArray]
---*/

var sab = new SharedArrayBuffer(1024);

var other_views = [Uint8ClampedArray, Float32Array, Float64Array];

testWithTypedArrayConstructors(function(View) {
  var view = new View(sab);

  assert.throws(TypeError, (() => Atomics.or(view, 0, 0)));
}, other_views);

reportCompare(0, 0);
