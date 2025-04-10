/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

"use strict";

ChromeUtils.import("resource://gre/modules/Preferences.jsm");
ChromeUtils.import("resource://gre/modules/Services.jsm");

const {InvalidArgumentError} = ChromeUtils.import("chrome://marionette/content/error.js", {});
ChromeUtils.import("chrome://marionette/content/session.js");

add_test(function test_Timeouts_ctor() {
  let ts = new session.Timeouts();
  equal(ts.implicit, 0);
  equal(ts.pageLoad, 300000);
  equal(ts.script, 30000);

  run_next_test();
});

add_test(function test_Timeouts_toString() {
  equal(new session.Timeouts().toString(), "[object session.Timeouts]");

  run_next_test();
});

add_test(function test_Timeouts_toJSON() {
  let ts = new session.Timeouts();
  deepEqual(ts.toJSON(), {"implicit": 0, "pageLoad": 300000, "script": 30000});

  run_next_test();
});

add_test(function test_Timeouts_fromJSON() {
  let json = {
    implicit: 10,
    pageLoad: 20,
    script: 30,
  };
  let ts = session.Timeouts.fromJSON(json);
  equal(ts.implicit, json.implicit);
  equal(ts.pageLoad, json.pageLoad);
  equal(ts.script, json.script);

  run_next_test();
});

add_test(function test_Timeouts_fromJSON_unrecognised_field() {
  let json = {
    sessionId: "foobar",
    script: 42,
  };
  try {
    session.Timeouts.fromJSON(json);
  } catch (e) {
    equal(e.name, InvalidArgumentError.name);
    equal(e.message, "Unrecognised timeout: sessionId");
  }

  run_next_test();
});

add_test(function test_Timeouts_fromJSON_invalid_type() {
  try {
    session.Timeouts.fromJSON({script: "foobar"});
  } catch (e) {
    equal(e.name, InvalidArgumentError.name);
    equal(e.message, "Expected [object String] \"foobar\" to be an integer");
  }

  run_next_test();
});

add_test(function test_Timeouts_fromJSON_bounds() {
  try {
    session.Timeouts.fromJSON({script: -42});
  } catch (e) {
    equal(e.name, InvalidArgumentError.name);
    equal(e.message, "Expected [object Number] -42 to be >= 0");
  }

  run_next_test();
});

add_test(function test_PageLoadStrategy() {
  equal(session.PageLoadStrategy.None, "none");
  equal(session.PageLoadStrategy.Eager, "eager");
  equal(session.PageLoadStrategy.Normal, "normal");

  run_next_test();
});

add_test(function test_Proxy_ctor() {
  let p = new session.Proxy();
  let props = [
    "proxyType",
    "httpProxy",
    "sslProxy",
    "ftpProxy",
    "socksProxy",
    "socksVersion",
    "proxyAutoconfigUrl",
  ];
  for (let prop of props) {
    ok(prop in p, `${prop} in ${JSON.stringify(props)}`);
    equal(p[prop], null);
  }

  run_next_test();
});

add_test(function test_Proxy_init() {
  let p = new session.Proxy();

  // no changed made, and 5 (system) is default
  equal(p.init(), false);
  equal(Preferences.get("network.proxy.type"), 5);

  // pac
  p.proxyType = "pac";
  p.proxyAutoconfigUrl = "http://localhost:1234";
  ok(p.init());

  equal(Preferences.get("network.proxy.type"), 2);
  equal(Preferences.get("network.proxy.autoconfig_url"),
      "http://localhost:1234");

  // direct
  p = new session.Proxy();
  p.proxyType = "direct";
  ok(p.init());
  equal(Preferences.get("network.proxy.type"), 0);

  // autodetect
  p = new session.Proxy();
  p.proxyType = "autodetect";
  ok(p.init());
  equal(Preferences.get("network.proxy.type"), 4);

  // system
  p = new session.Proxy();
  p.proxyType = "system";
  ok(p.init());
  equal(Preferences.get("network.proxy.type"), 5);

  // manual
  for (let proxy of ["ftp", "http", "ssl", "socks"]) {
    p = new session.Proxy();
    p.proxyType = "manual";
    p.noProxy = ["foo", "bar"];
    p[`${proxy}Proxy`] = "foo";
    p[`${proxy}ProxyPort`] = 42;
    if (proxy === "socks") {
      p[`${proxy}Version`] = 4;
    }

    ok(p.init());
    equal(Preferences.get("network.proxy.type"), 1);
    equal(Preferences.get("network.proxy.no_proxies_on"), "foo, bar");
    equal(Preferences.get(`network.proxy.${proxy}`), "foo");
    equal(Preferences.get(`network.proxy.${proxy}_port`), 42);
    if (proxy === "socks") {
      equal(Preferences.get(`network.proxy.${proxy}_version`), 4);
    }
  }

  // empty no proxy should reset default exclustions
  p = new session.Proxy();
  p.proxyType = "manual";
  p.noProxy = [];
  ok(p.init());
  equal(Preferences.get("network.proxy.no_proxies_on"), "");

  run_next_test();
});

add_test(function test_Proxy_toString() {
  equal(new session.Proxy().toString(), "[object session.Proxy]");

  run_next_test();
});

add_test(function test_Proxy_toJSON() {
  let p = new session.Proxy();
  deepEqual(p.toJSON(), {});

  // autoconfig url
  p = new session.Proxy();
  p.proxyType = "pac";
  p.proxyAutoconfigUrl = "foo";
  deepEqual(p.toJSON(), {proxyType: "pac", proxyAutoconfigUrl: "foo"});

  // manual proxy
  p = new session.Proxy();
  p.proxyType = "manual";
  deepEqual(p.toJSON(), {proxyType: "manual"});

  for (let proxy of ["ftpProxy", "httpProxy", "sslProxy", "socksProxy"]) {
    let expected = {proxyType: "manual"};

    p = new session.Proxy();
    p.proxyType = "manual";

    if (proxy == "socksProxy") {
      p.socksVersion = 5;
      expected.socksVersion = 5;
    }

    // without port
    p[proxy] = "foo";
    expected[proxy] = "foo";
    deepEqual(p.toJSON(), expected);

    // with port
    p[proxy] = "foo";
    p[`${proxy}Port`] = 0;
    expected[proxy] = "foo:0";
    deepEqual(p.toJSON(), expected);

    p[`${proxy}Port`] = 42;
    expected[proxy] = "foo:42";
    deepEqual(p.toJSON(), expected);

    // add brackets for IPv6 address as proxy hostname
    p[proxy] = "2001:db8::1";
    p[`${proxy}Port`] = 42;
    expected[proxy] = "foo:42";
    expected[proxy] = "[2001:db8::1]:42";
    deepEqual(p.toJSON(), expected);
  }

  // noProxy: add brackets for IPv6 address
  p = new session.Proxy();
  p.proxyType = "manual";
  p.noProxy = ["2001:db8::1"];
  let expected = {proxyType: "manual", noProxy: "[2001:db8::1]"};
  deepEqual(p.toJSON(), expected);

  run_next_test();
});

add_test(function test_Proxy_fromJSON() {
  let p = new session.Proxy();
  deepEqual(p, session.Proxy.fromJSON(undefined));
  deepEqual(p, session.Proxy.fromJSON(null));

  for (let typ of [true, 42, "foo", []]) {
    Assert.throws(() => session.Proxy.fromJSON(typ), /InvalidArgumentError/);
  }

  // must contain a valid proxyType
  Assert.throws(() => session.Proxy.fromJSON({}), /InvalidArgumentError/);
  Assert.throws(() => session.Proxy.fromJSON({proxyType: "foo"}),
      /InvalidArgumentError/);

  // autoconfig url
  for (let url of [true, 42, [], {}]) {
    Assert.throws(() => session.Proxy.fromJSON(
        {proxyType: "pac", proxyAutoconfigUrl: url}), /InvalidArgumentError/);
  }

  p = new session.Proxy();
  p.proxyType = "pac";
  p.proxyAutoconfigUrl = "foo";
  deepEqual(p,
      session.Proxy.fromJSON({proxyType: "pac", proxyAutoconfigUrl: "foo"}));

  // manual proxy
  p = new session.Proxy();
  p.proxyType = "manual";
  deepEqual(p, session.Proxy.fromJSON({proxyType: "manual"}));

  for (let proxy of ["httpProxy", "sslProxy", "ftpProxy", "socksProxy"]) {
    let manual = {proxyType: "manual"};

    // invalid hosts
    for (let host of [true, 42, [], {}, null, "http://foo",
      "foo:-1", "foo:65536", "foo/test", "foo#42", "foo?foo=bar",
      "2001:db8::1"]) {
      manual[proxy] = host;
      Assert.throws(() => session.Proxy.fromJSON(manual),
          /InvalidArgumentError/);
    }

    p = new session.Proxy();
    p.proxyType = "manual";
    if (proxy == "socksProxy") {
      manual.socksVersion = 5;
      p.socksVersion = 5;
    }

    let host_map = {
      "foo:1": {hostname: "foo", port: 1},
      "foo:21": {hostname: "foo", port: 21},
      "foo:80": {hostname: "foo", port: 80},
      "foo:443": {hostname: "foo", port: 443},
      "foo:65535": {hostname: "foo", port: 65535},
      "127.0.0.1:42": {hostname: "127.0.0.1", port: 42},
      "[2001:db8::1]:42": {hostname: "2001:db8::1", port: "42"},
    };

    // valid proxy hosts with port
    for (let host in host_map) {
      manual[proxy] = host;

      p[`${proxy}`] = host_map[host].hostname;
      p[`${proxy}Port`] = host_map[host].port;

      deepEqual(p, session.Proxy.fromJSON(manual));
    }

    // Without a port the default port of the scheme is used
    for (let host of ["foo", "foo:"]) {
      manual[proxy] = host;

      // For socks no default port is available
      p[proxy] = `foo`;
      if (proxy === "socksProxy") {
        p[`${proxy}Port`] = null;
      } else {
        let default_ports = {"ftpProxy": 21, "httpProxy": 80,
          "sslProxy": 443};

        p[`${proxy}Port`] = default_ports[proxy];
      }

      deepEqual(p, session.Proxy.fromJSON(manual));
    }
  }

  // missing required socks version
  Assert.throws(() => session.Proxy.fromJSON(
      {proxyType: "manual", socksProxy: "foo:1234"}),
      /InvalidArgumentError/);

  // noProxy: invalid settings
  for (let noProxy of [true, 42, {}, null, "foo",
      [true], [42], [{}], [null]]) {
    Assert.throws(() => session.Proxy.fromJSON(
        {proxyType: "manual", noProxy}),
        /InvalidArgumentError/);
  }

  // noProxy: valid settings
  p = new session.Proxy();
  p.proxyType = "manual";
  for (let noProxy of [[], ["foo"], ["foo", "bar"], ["127.0.0.1"]]) {
    let manual = {proxyType: "manual", "noProxy": noProxy};
    p.noProxy = noProxy;
    deepEqual(p, session.Proxy.fromJSON(manual));
  }

  // noProxy: IPv6 needs brackets removed
  p = new session.Proxy();
  p.proxyType = "manual";
  p.noProxy = ["2001:db8::1"];
  let manual = {proxyType: "manual", "noProxy": ["[2001:db8::1]"]};
  deepEqual(p, session.Proxy.fromJSON(manual));

  run_next_test();
});

add_test(function test_Capabilities_ctor() {
  let caps = new session.Capabilities();
  ok(caps.has("browserName"));
  ok(caps.has("browserVersion"));
  ok(caps.has("platformName"));
  ok(caps.has("platformVersion"));
  equal(session.PageLoadStrategy.Normal, caps.get("pageLoadStrategy"));
  equal(false, caps.get("acceptInsecureCerts"));
  ok(caps.get("timeouts") instanceof session.Timeouts);
  ok(caps.get("proxy") instanceof session.Proxy);

  ok(caps.has("rotatable"));

  equal(false, caps.get("moz:accessibilityChecks"));
  ok(caps.has("moz:processID"));
  ok(caps.has("moz:profile"));
  equal(false, caps.get("moz:useNonSpecCompliantPointerOrigin"));
  equal(true, caps.get("moz:webdriverClick"));

  run_next_test();
});

add_test(function test_Capabilities_toString() {
  equal("[object session.Capabilities]", new session.Capabilities().toString());

  run_next_test();
});

add_test(function test_Capabilities_toJSON() {
  let caps = new session.Capabilities();
  let json = caps.toJSON();

  equal(caps.get("browserName"), json.browserName);
  equal(caps.get("browserVersion"), json.browserVersion);
  equal(caps.get("platformName"), json.platformName);
  equal(caps.get("platformVersion"), json.platformVersion);
  equal(caps.get("pageLoadStrategy"), json.pageLoadStrategy);
  equal(caps.get("acceptInsecureCerts"), json.acceptInsecureCerts);
  deepEqual(caps.get("timeouts").toJSON(), json.timeouts);
  equal(undefined, json.proxy);

  equal(caps.get("rotatable"), json.rotatable);

  equal(caps.get("moz:accessibilityChecks"), json["moz:accessibilityChecks"]);
  equal(caps.get("moz:processID"), json["moz:processID"]);
  equal(caps.get("moz:profile"), json["moz:profile"]);
  equal(caps.get("moz:useNonSpecCompliantPointerOrigin"),
      json["moz:useNonSpecCompliantPointerOrigin"]);
  equal(caps.get("moz:webdriverClick"), json["moz:webdriverClick"]);

  run_next_test();
});

add_test(function test_Capabilities_fromJSON() {
  const {fromJSON} = session.Capabilities;

  // plain
  for (let typ of [{}, null, undefined]) {
    ok(fromJSON(typ).has("browserName"));
  }
  for (let typ of [true, 42, "foo", []]) {
    Assert.throws(() => fromJSON(typ), /InvalidArgumentError/);
  }

  // matching
  let caps = new session.Capabilities();

  caps = fromJSON({acceptInsecureCerts: true});
  equal(true, caps.get("acceptInsecureCerts"));
  caps = fromJSON({acceptInsecureCerts: false});
  equal(false, caps.get("acceptInsecureCerts"));
  Assert.throws(() => fromJSON({acceptInsecureCerts: "foo"}));

  for (let strategy of Object.values(session.PageLoadStrategy)) {
    caps = fromJSON({pageLoadStrategy: strategy});
    equal(strategy, caps.get("pageLoadStrategy"));
  }
  Assert.throws(() => fromJSON({pageLoadStrategy: "foo"}));

  let proxyConfig = {proxyType: "manual"};
  caps = fromJSON({proxy: proxyConfig});
  equal("manual", caps.get("proxy").proxyType);

  let timeoutsConfig = {implicit: 123};
  caps = fromJSON({timeouts: timeoutsConfig});
  equal(123, caps.get("timeouts").implicit);

  caps = fromJSON({"moz:accessibilityChecks": true});
  equal(true, caps.get("moz:accessibilityChecks"));
  caps = fromJSON({"moz:accessibilityChecks": false});
  equal(false, caps.get("moz:accessibilityChecks"));
  Assert.throws(() => fromJSON({"moz:accessibilityChecks": "foo"}));
  Assert.throws(() => fromJSON({"moz:accessibilityChecks": 1}));

  caps = fromJSON({"moz:useNonSpecCompliantPointerOrigin": false});
  equal(false, caps.get("moz:useNonSpecCompliantPointerOrigin"));
  caps = fromJSON({"moz:useNonSpecCompliantPointerOrigin": true});
  equal(true, caps.get("moz:useNonSpecCompliantPointerOrigin"));
  Assert.throws(() => fromJSON({"moz:useNonSpecCompliantPointerOrigin": "foo"}));
  Assert.throws(() => fromJSON({"moz:useNonSpecCompliantPointerOrigin": 1}));

  caps = fromJSON({"moz:webdriverClick": true});
  equal(true, caps.get("moz:webdriverClick"));
  caps = fromJSON({"moz:webdriverClick": false});
  equal(false, caps.get("moz:webdriverClick"));
  Assert.throws(() => fromJSON({"moz:webdriverClick": "foo"}));
  Assert.throws(() => fromJSON({"moz:webdriverClick": 1}));

  run_next_test();
});

// use session.Proxy.toJSON to test marshal
add_test(function test_marshal() {
  let proxy = new session.Proxy();

  // drop empty fields
  deepEqual({}, proxy.toJSON());
  proxy.proxyType = "manual";
  deepEqual({proxyType: "manual"}, proxy.toJSON());
  proxy.proxyType = null;
  deepEqual({}, proxy.toJSON());
  proxy.proxyType = undefined;
  deepEqual({}, proxy.toJSON());

  // iterate over object literals
  proxy.proxyType = {foo: "bar"};
  deepEqual({proxyType: {foo: "bar"}}, proxy.toJSON());

  // iterate over complex object that implement toJSON
  proxy.proxyType = new session.Proxy();
  deepEqual({}, proxy.toJSON());
  proxy.proxyType.proxyType = "manual";
  deepEqual({proxyType: {proxyType: "manual"}}, proxy.toJSON());

  // drop objects with no entries
  proxy.proxyType = {foo: {}};
  deepEqual({}, proxy.toJSON());
  proxy.proxyType = {foo: new session.Proxy()};
  deepEqual({}, proxy.toJSON());

  run_next_test();
});
