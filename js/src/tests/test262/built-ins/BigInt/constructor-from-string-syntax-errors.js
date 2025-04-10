// |reftest| skip-if(!this.hasOwnProperty('BigInt')) -- BigInt is not enabled unconditionally
// Copyright (C) 2017 Caio Lima. All rights reserved.
// This code is governed by the BSD license found in the LICENSE file.

/*---
description: Invalid String into BigInt constructor should throw SyntaxError
esid: sec-string-to-bigint
info: |
  Apply the algorithm in 3.1.3.1 with the following changes:

  - Replace the StrUnsignedDecimalLiteral production with DecimalDigits
    to not allow decimal points or exponents.

features: [BigInt]
---*/

assert.throws(SyntaxError, function() {
  BigInt("10n");
});

assert.throws(SyntaxError, function() {
  BigInt("10x");
});

assert.throws(SyntaxError, function() {
  BigInt("10b");
});

assert.throws(SyntaxError, function() {
  BigInt("10.5");
});

assert.throws(SyntaxError, function() {
  BigInt("0b");
});

assert.throws(SyntaxError, function() {
  BigInt("-0x1");
});

assert.throws(SyntaxError, function() {
  BigInt("-0XFFab");
});

assert.throws(SyntaxError, function() {
  BigInt("0oa");
});

assert.throws(SyntaxError, function() {
  BigInt("000 12");
});

assert.throws(SyntaxError, function() {
  BigInt("0o");
});

assert.throws(SyntaxError, function() {
  BigInt("0x");
});

assert.throws(SyntaxError, function() {
  BigInt("00o");
});

assert.throws(SyntaxError, function() {
  BigInt("00b");
});

assert.throws(SyntaxError, function() {
  BigInt("00x");
});

reportCompare(0, 0);
