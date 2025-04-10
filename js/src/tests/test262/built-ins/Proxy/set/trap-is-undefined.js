// Copyright (C) 2015 the V8 project authors. All rights reserved.
// This code is governed by the BSD license found in the LICENSE file.
/*---
es6id: 9.5.9
description: >
    [[Set]] ( P, V, Receiver)

    8. If trap is undefined, then return target.[[Set]](P, V, Receiver).

---*/

var target = {
  attr: 1
};
var p = new Proxy(target, {
  get: undefined
});

p.attr = 1;

assert.sameValue(target.attr, 1);

reportCompare(0, 0);
