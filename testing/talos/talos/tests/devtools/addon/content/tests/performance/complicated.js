/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

"use strict";

const { openToolboxAndLog, closeToolboxAndLog, reloadPageAndLog, testSetup,
        testTeardown, COMPLICATED_URL } = require("../head");

module.exports = async function() {
  await testSetup(COMPLICATED_URL);
  const toolbox = await openToolboxAndLog("complicated.performance", "performance");
  await reloadPageAndLog("complicated.performance", toolbox);
  await closeToolboxAndLog("complicated.performance", toolbox);
  await testTeardown();
};
