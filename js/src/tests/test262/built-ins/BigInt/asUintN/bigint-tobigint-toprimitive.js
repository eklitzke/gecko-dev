// |reftest| skip-if(!this.hasOwnProperty('BigInt')) -- BigInt is not enabled unconditionally
// Copyright (C) 2017 Josh Wolfe. All rights reserved.
// This code is governed by the BSD license found in the LICENSE file.
/*---
description: BigInt.asUintN type coercion for bigint parameter
esid: pending
info: |
  BigInt.asUintN ( bits, bigint )

  2. Let bigint ? ToBigInt(bigint).
features: [BigInt, Symbol.toPrimitive, computed-property-names]
---*/

function err() {
  throw new Test262Error();
}

function MyError() {}

assert.sameValue(BigInt.asUintN(2, {
  [Symbol.toPrimitive]: function() {
    return "1";
  },
  valueOf: err,
  toString: err
}), 1n, "ToPrimitive: @@toPrimitive takes precedence");
assert.sameValue(BigInt.asUintN(2, {
  valueOf: function() {
    return "1";
  },
  toString: err
}), 1n, "ToPrimitive: valueOf takes precedence over toString");
assert.sameValue(BigInt.asUintN(2, {
  toString: function() {
    return "1";
  }
}), 1n, "ToPrimitive: toString with no valueOf");
assert.sameValue(BigInt.asUintN(2, {
  [Symbol.toPrimitive]: undefined,
  valueOf: function() {
    return "1";
  }
}), 1n, "ToPrimitive: skip @@toPrimitive when it's undefined");
assert.sameValue(BigInt.asUintN(2, {
  [Symbol.toPrimitive]: null,
  valueOf: function() {
    return "1";
  }
}), 1n, "ToPrimitive: skip @@toPrimitive when it's null");
assert.sameValue(BigInt.asUintN(2, {
  valueOf: null,
  toString: function() {
    return "1";
  }
}), 1n, "ToPrimitive: skip valueOf when it's not callable");
assert.sameValue(BigInt.asUintN(2, {
  valueOf: 1,
  toString: function() {
    return "1";
  }
}), 1n, "ToPrimitive: skip valueOf when it's not callable");
assert.sameValue(BigInt.asUintN(2, {
  valueOf: {},
  toString: function() {
    return "1";
  }
}), 1n, "ToPrimitive: skip valueOf when it's not callable");
assert.sameValue(BigInt.asUintN(2, {
  valueOf: function() {
    return {};
  },
  toString: function() {
    return "1";
  }
}), 1n, "ToPrimitive: skip valueOf when it returns an object");
assert.sameValue(BigInt.asUintN(2, {
  valueOf: function() {
    return Object(12345);
  },
  toString: function() {
    return "1";
  }
}), 1n, "ToPrimitive: skip valueOf when it returns an object");
assert.throws(TypeError, function() {
  BigInt.asUintN(0, {
    [Symbol.toPrimitive]: 1
  });
}, "ToPrimitive: throw when @@toPrimitive is not callable");
assert.throws(TypeError, function() {
  BigInt.asUintN(0, {
    [Symbol.toPrimitive]: {}
  });
}, "ToPrimitive: throw when @@toPrimitive is not callable");
assert.throws(TypeError, function() {
  BigInt.asUintN(0, {
    [Symbol.toPrimitive]: function() {
      return Object(1);
    }
  });
}, "ToPrimitive: throw when @@toPrimitive returns an object");
assert.throws(TypeError, function() {
  BigInt.asUintN(0, {
    [Symbol.toPrimitive]: function() {
      return {};
    }
  });
}, "ToPrimitive: throw when @@toPrimitive returns an object");
assert.throws(MyError, function() {
  BigInt.asUintN(0, {
    [Symbol.toPrimitive]: function() {
      throw new MyError();
    }
  });
}, "ToPrimitive: propagate errors from @@toPrimitive");
assert.throws(MyError, function() {
  BigInt.asUintN(0, {
    valueOf: function() {
      throw new MyError();
    }
  });
}, "ToPrimitive: propagate errors from valueOf");
assert.throws(MyError, function() {
  BigInt.asUintN(0, {
    toString: function() {
      throw new MyError();
    }
  });
}, "ToPrimitive: propagate errors from toString");
assert.throws(TypeError, function() {
  BigInt.asUintN(0, {
    valueOf: null,
    toString: null
  });
}, "ToPrimitive: throw when skipping both valueOf and toString");
assert.throws(TypeError, function() {
  BigInt.asUintN(0, {
    valueOf: 1,
    toString: 1
  });
}, "ToPrimitive: throw when skipping both valueOf and toString");
assert.throws(TypeError, function() {
  BigInt.asUintN(0, {
    valueOf: {},
    toString: {}
  });
}, "ToPrimitive: throw when skipping both valueOf and toString");
assert.throws(TypeError, function() {
  BigInt.asUintN(0, {
    valueOf: function() {
      return Object(1);
    },
    toString: function() {
      return Object(1);
    }
  });
}, "ToPrimitive: throw when skipping both valueOf and toString");
assert.throws(TypeError, function() {
  BigInt.asUintN(0, {
    valueOf: function() {
      return {};
    },
    toString: function() {
      return {};
    }
  });
}, "ToPrimitive: throw when skipping both valueOf and toString");

reportCompare(0, 0);
