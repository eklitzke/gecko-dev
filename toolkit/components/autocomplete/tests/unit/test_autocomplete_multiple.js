/* -*- indent-tabs-mode: nil; js-indent-level: 2 -*- */
/* vim:set ts=2 sw=2 sts=2 et: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/**
 * Dummy nsIAutoCompleteInput source that returns
 * the given list of AutoCompleteSearch names.
 *
 * Implements only the methods needed for this test.
 */
function AutoCompleteInput(aSearches) {
  this.searches = aSearches;
}
AutoCompleteInput.prototype = {
  constructor: AutoCompleteInput,

  // Array of AutoCompleteSearch names
  searches: null,

  minResultsForPopup: 0,
  timeout: 10,
  searchParam: "",
  textValue: "",
  disableAutoComplete: false,
  completeDefaultIndex: false,

  get searchCount() {
    return this.searches.length;
  },

  getSearchAt(aIndex) {
    return this.searches[aIndex];
  },

  onSearchBegin() {},
  onSearchComplete() {},

  popupOpen: false,

  popup: {
    setSelectedIndex(aIndex) {},
    invalidate() {},

    // nsISupports implementation
    QueryInterface: ChromeUtils.generateQI(["nsIAutoCompletePopup"])
  },

  // nsISupports implementation
  QueryInterface: ChromeUtils.generateQI(["nsIAutoCompleteInput"])
};



/**
 * nsIAutoCompleteResult implementation
 */
function AutoCompleteResult(aValues, aComments, aStyles) {
  this._values = aValues;
  this._comments = aComments;
  this._styles = aStyles;
}
AutoCompleteResult.prototype = {
  constructor: AutoCompleteResult,

  // Arrays
  _values: null,
  _comments: null,
  _styles: null,

  searchString: "",
  searchResult: null,

  defaultIndex: 0,

  get matchCount() {
    return this._values.length;
  },

  getValueAt(aIndex) {
    return this._values[aIndex];
  },

  getLabelAt(aIndex) {
    return this.getValueAt(aIndex);
  },

  getCommentAt(aIndex) {
    return this._comments[aIndex];
  },

  getStyleAt(aIndex) {
    return this._styles[aIndex];
  },

  getImageAt(aIndex) {
    return "";
  },

  getFinalCompleteValueAt(aIndex) {
    return this.getValueAt(aIndex);
  },

  removeValueAt(aRowIndex, aRemoveFromDb) {},

  // nsISupports implementation
  QueryInterface: ChromeUtils.generateQI(["nsIAutoCompleteResult"])
};



/**
 * nsIAutoCompleteSearch implementation that always returns
 * the same result set.
 */
function AutoCompleteSearch(aName, aResult) {
  this.name = aName;
  this._result = aResult;
}
AutoCompleteSearch.prototype = {
  constructor: AutoCompleteSearch,

  // Search name. Used by AutoCompleteController
  name: null,

  // AutoCompleteResult
  _result: null,


  /**
   * Return the same result set for every search
   */
  startSearch(aSearchString,
                        aSearchParam,
                        aPreviousResult,
                        aListener) {
    var result = this._result;
    if (result._values.length > 0) {
      result.searchResult = Ci.nsIAutoCompleteResult.RESULT_SUCCESS_ONGOING;
    } else {
      result.searchResult = Ci.nsIAutoCompleteResult.RESULT_NOMATCH_ONGOING;
    }
    aListener.onSearchResult(this, result);

    if (result._values.length > 0) {
      result.searchResult = Ci.nsIAutoCompleteResult.RESULT_SUCCESS;
    } else {
      result.searchResult = Ci.nsIAutoCompleteResult.RESULT_NOMATCH;
    }
    aListener.onSearchResult(this, result);
  },

  stopSearch() {},

  // nsISupports implementation
  QueryInterface: ChromeUtils.generateQI(["nsIFactory", "nsIAutoCompleteSearch"]),

  // nsIFactory implementation
  createInstance(outer, iid) {
    return this.QueryInterface(iid);
  }
};



/**
 * Helper to register an AutoCompleteSearch with the given name.
 * Allows the AutoCompleteController to find the search.
 */
function registerAutoCompleteSearch(aSearch) {
  var name = "@mozilla.org/autocomplete/search;1?name=" + aSearch.name;

  var uuidGenerator = Cc["@mozilla.org/uuid-generator;1"].
                      getService(Ci.nsIUUIDGenerator);
  var cid = uuidGenerator.generateUUID();

  var desc = "Test AutoCompleteSearch";

  var componentManager = Components.manager
                                   .QueryInterface(Ci.nsIComponentRegistrar);
  componentManager.registerFactory(cid, desc, name, aSearch);

  // Keep the id on the object so we can unregister later
  aSearch.cid = cid;
}



/**
 * Helper to unregister an AutoCompleteSearch.
 */
function unregisterAutoCompleteSearch(aSearch) {
  var componentManager = Components.manager
                                   .QueryInterface(Ci.nsIComponentRegistrar);
  componentManager.unregisterFactory(aSearch.cid, aSearch);
}



/**
 * Test AutoComplete with multiple AutoCompleteSearch sources.
 */
function run_test() {
  var expected1 = ["1", "2", "3"];
  var expected2 = ["a", "b", "c"];
  var search1 = new AutoCompleteSearch("search1",
                             new AutoCompleteResult(expected1, [], []));
  var search2 = new AutoCompleteSearch("search2",
                             new AutoCompleteResult(expected2, [], []));

  // Register searches so AutoCompleteController can find them
  registerAutoCompleteSearch(search1);
  registerAutoCompleteSearch(search2);

  var controller = Cc["@mozilla.org/autocomplete/controller;1"].
                   getService(Ci.nsIAutoCompleteController);

  // Make an AutoCompleteInput that uses our searches
  // and confirms results on search complete
  var input = new AutoCompleteInput([search1.name, search2.name]);
  var numSearchesStarted = 0;

  input.onSearchBegin = function() {
    numSearchesStarted++;
    Assert.equal(numSearchesStarted, 1);
  };

  input.onSearchComplete = function() {

    Assert.equal(numSearchesStarted, 1);

    Assert.equal(controller.searchStatus,
                 Ci.nsIAutoCompleteController.STATUS_COMPLETE_MATCH);
    Assert.equal(controller.matchCount, expected1.length + expected2.length);

    // Unregister searches
    unregisterAutoCompleteSearch(search1);
    unregisterAutoCompleteSearch(search2);

    do_test_finished();
  };

  controller.input = input;

  // Search is asynchronous, so don't let the test finish immediately
  do_test_pending();

  controller.startSearch("test");
}
