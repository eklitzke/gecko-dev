"use strict";

Object.defineProperty(exports, "__esModule", {
  value: true
});
exports.getHiddenTabs = getHiddenTabs;
exports.getSourceAnnotation = getSourceAnnotation;
exports.getTabMenuItems = getTabMenuItems;

var _react = require("devtools/client/shared/vendor/react");

var _react2 = _interopRequireDefault(_react);

var _source = require("./source");

function _interopRequireDefault(obj) { return obj && obj.__esModule ? obj : { default: obj }; }

/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at <http://mozilla.org/MPL/2.0/>. */

/*
 * Finds the hidden tabs by comparing the tabs' top offset.
 * hidden tabs will have a great top offset.
 *
 * @param sourceTabs Immutable.list
 * @param sourceTabEls HTMLCollection
 *
 * @returns Immutable.list
 */
function getHiddenTabs(sourceTabs, sourceTabEls) {
  sourceTabEls = [].slice.call(sourceTabEls);

  function getTopOffset() {
    const topOffsets = sourceTabEls.map(t => t.getBoundingClientRect().top);
    return Math.min(...topOffsets);
  }

  function hasTopOffset(el) {
    // adding 10px helps account for cases where the tab might be offset by
    // styling such as selected tabs which don't have a border.
    const tabTopOffset = getTopOffset();
    return el.getBoundingClientRect().top > tabTopOffset + 10;
  }

  return sourceTabs.filter((tab, index) => {
    const element = sourceTabEls[index];
    return element && hasTopOffset(element);
  });
}

function getSourceAnnotation(source, sourceMetaData) {
  const framework = sourceMetaData && sourceMetaData.framework ? sourceMetaData.framework : false;

  if (framework) {
    return _react2.default.createElement("img", {
      className: framework.toLowerCase()
    });
  }

  if ((0, _source.isPretty)(source)) {
    return _react2.default.createElement("img", {
      className: "prettyPrint"
    });
  }

  if (source.isBlackBoxed) {
    return _react2.default.createElement("img", {
      className: "blackBox"
    });
  }
}

function getTabMenuItems() {
  return {
    closeTab: {
      id: "node-menu-close-tab",
      label: L10N.getStr("sourceTabs.closeTab"),
      accesskey: L10N.getStr("sourceTabs.closeTab.accesskey"),
      disabled: false
    },
    closeOtherTabs: {
      id: "node-menu-close-other-tabs",
      label: L10N.getStr("sourceTabs.closeOtherTabs"),
      accesskey: L10N.getStr("sourceTabs.closeOtherTabs.accesskey"),
      disabled: false
    },
    closeTabsToEnd: {
      id: "node-menu-close-tabs-to-end",
      label: L10N.getStr("sourceTabs.closeTabsToEnd"),
      accesskey: L10N.getStr("sourceTabs.closeTabsToEnd.accesskey"),
      disabled: false
    },
    closeAllTabs: {
      id: "node-menu-close-all-tabs",
      label: L10N.getStr("sourceTabs.closeAllTabs"),
      accesskey: L10N.getStr("sourceTabs.closeAllTabs.accesskey"),
      disabled: false
    },
    showSource: {
      id: "node-menu-show-source",
      label: L10N.getStr("sourceTabs.revealInTree"),
      accesskey: L10N.getStr("sourceTabs.revealInTree.accesskey"),
      disabled: false
    },
    copyToClipboard: {
      id: "node-menu-copy-to-clipboard",
      label: L10N.getStr("copyToClipboard.label"),
      accesskey: L10N.getStr("copyToClipboard.accesskey"),
      disabled: false
    },
    copySourceUri2: {
      id: "node-menu-copy-source-url",
      label: L10N.getStr("copySourceUri2"),
      accesskey: L10N.getStr("copySourceUri2.accesskey"),
      disabled: false
    },
    prettyPrint: {
      id: "node-menu-pretty-print",
      label: L10N.getStr("sourceTabs.prettyPrint"),
      accesskey: L10N.getStr("sourceTabs.prettyPrint.accesskey"),
      disabled: false
    }
  };
}