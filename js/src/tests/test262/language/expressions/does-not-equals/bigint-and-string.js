// |reftest| skip-if(!this.hasOwnProperty('BigInt')) -- BigInt is not enabled unconditionally
// Copyright (C) 2017 Josh Wolfe. All rights reserved.
// This code is governed by the BSD license found in the LICENSE file.

/*---
description: Non-strict inequality comparison of BigInt and String values
esid: sec-abstract-equality-comparison
info: |
  1. If Type(x) is different from Type(y), return false.

features: [BigInt]
---*/

assert.sameValue(0n != "", false, '0n != ""');
assert.sameValue("" != 0n, false, '"" != 0n');
assert.sameValue(0n != "-0", false, '0n != "-0"');
assert.sameValue("-0" != 0n, false, '"-0" != 0n');
assert.sameValue(0n != "0", false, '0n != "0"');
assert.sameValue("0" != 0n, false, '"0" != 0n');
assert.sameValue(0n != "-1", true, '0n != "-1"');
assert.sameValue("-1" != 0n, true, '"-1" != 0n');
assert.sameValue(0n != "1", true, '0n != "1"');
assert.sameValue("1" != 0n, true, '"1" != 0n');
assert.sameValue(0n != "foo", true, '0n != "foo"');
assert.sameValue("foo" != 0n, true, '"foo" != 0n');
assert.sameValue(1n != "", true, '1n != ""');
assert.sameValue("" != 1n, true, '"" != 1n');
assert.sameValue(1n != "-0", true, '1n != "-0"');
assert.sameValue("-0" != 1n, true, '"-0" != 1n');
assert.sameValue(1n != "0", true, '1n != "0"');
assert.sameValue("0" != 1n, true, '"0" != 1n');
assert.sameValue(1n != "-1", true, '1n != "-1"');
assert.sameValue("-1" != 1n, true, '"-1" != 1n');
assert.sameValue(1n != "1", false, '1n != "1"');
assert.sameValue("1" != 1n, false, '"1" != 1n');
assert.sameValue(1n != "foo", true, '1n != "foo"');
assert.sameValue("foo" != 1n, true, '"foo" != 1n');
assert.sameValue(-1n != "-", true, '-1n != "-"');
assert.sameValue("-" != -1n, true, '"-" != -1n');
assert.sameValue(-1n != "-0", true, '-1n != "-0"');
assert.sameValue("-0" != -1n, true, '"-0" != -1n');
assert.sameValue(-1n != "-1", false, '-1n != "-1"');
assert.sameValue("-1" != -1n, false, '"-1" != -1n');
assert.sameValue(-1n != "-foo", true, '-1n != "-foo"');
assert.sameValue("-foo" != -1n, true, '"-foo" != -1n');
assert.sameValue(900719925474099101n != "900719925474099101", false, '900719925474099101n != "900719925474099101"');
assert.sameValue("900719925474099101" != 900719925474099101n, false, '"900719925474099101" != 900719925474099101n');
assert.sameValue(900719925474099102n != "900719925474099101", true, '900719925474099102n != "900719925474099101"');
assert.sameValue("900719925474099101" != 900719925474099102n, true, '"900719925474099101" != 900719925474099102n');

reportCompare(0, 0);
