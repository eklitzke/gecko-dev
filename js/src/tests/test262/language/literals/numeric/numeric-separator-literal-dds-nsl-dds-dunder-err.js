// |reftest| skip error:SyntaxError -- numeric-separator-literal is not supported
// Copyright (C) 2017 the V8 project authors. All rights reserved.
// This code is governed by the BSD license found in the LICENSE file.

/*---
esid: prod-NumericLiteralSeparator
description: >
  NumericLiteralSeparator may not appear adjacent to another
  NumericLiteralSeparator in DecimalIntegerLiteral
info: |
  NumericLiteralSeparator ::
    _

  DecimalIntegerLiteral ::
    ...
    NonZeroDigit NumericLiteralSeparator_opt DecimalDigits

negative:
  phase: early
  type: SyntaxError

features: [numeric-separator-literal]
---*/

throw "Test262: This statement should not be evaluated.";

10__0123456789
