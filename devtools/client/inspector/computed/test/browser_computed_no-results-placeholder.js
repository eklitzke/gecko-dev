/* vim: set ft=javascript ts=2 et sw=2 tw=80: */
/* Any copyright is dedicated to the Public Domain.
 http://creativecommons.org/publicdomain/zero/1.0/ */

"use strict";

// Tests that the no results placeholder works properly.

const TEST_URI = `
  <style type="text/css">
    .matches {
      color: #F00;
    }
  </style>
  <span id="matches" class="matches">Some styled text</span>
`;

add_task(async function() {
  await addTab("data:text/html;charset=utf-8," + encodeURIComponent(TEST_URI));
  let {inspector, view} = await openComputedView();
  await selectNode("#matches", inspector);

  await enterInvalidFilter(inspector, view);
  checkNoResultsPlaceholderShown(view);

  await clearFilterText(inspector, view);
  checkNoResultsPlaceholderHidden(view);
});

async function enterInvalidFilter(inspector, computedView) {
  let searchbar = computedView.searchField;
  let searchTerm = "xxxxx";

  info("setting filter text to \"" + searchTerm + "\"");

  let onRefreshed = inspector.once("computed-view-refreshed");
  searchbar.focus();
  synthesizeKeys(searchTerm, computedView.styleWindow);
  await onRefreshed;
}

function checkNoResultsPlaceholderShown(computedView) {
  info("Checking that the no results placeholder is shown");

  let placeholder = computedView.noResults;
  let win = computedView.styleWindow;
  let display = win.getComputedStyle(placeholder).display;
  is(display, "block", "placeholder is visible");
}

async function clearFilterText(inspector, computedView) {
  info("Clearing the filter text");

  let searchbar = computedView.searchField;

  let onRefreshed = inspector.once("computed-view-refreshed");
  searchbar.focus();
  searchbar.value = "";
  EventUtils.synthesizeKey("c", {}, computedView.styleWindow);
  await onRefreshed;
}

function checkNoResultsPlaceholderHidden(computedView) {
  info("Checking that the no results placeholder is hidden");

  let placeholder = computedView.noResults;
  let win = computedView.styleWindow;
  let display = win.getComputedStyle(placeholder).display;
  is(display, "none", "placeholder is hidden");
}
