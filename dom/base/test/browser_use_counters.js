/* -*- Mode: javascript; tab-width: 2; indent-tabs-mode: nil; js-indent-level: 2 -*- */

requestLongerTimeout(2);

ChromeUtils.import("resource://gre/modules/Services.jsm");

const gHttpTestRoot = "http://example.com/browser/dom/base/test/";

/**
 * Enable local telemetry recording for the duration of the tests.
 */
var gOldContentCanRecord = false;
var gOldParentCanRecord = false;
add_task(async function test_initialize() {
  let Telemetry = Cc["@mozilla.org/base/telemetry;1"].getService(Ci.nsITelemetry);
  gOldParentCanRecord = Telemetry.canRecordExtended
  Telemetry.canRecordExtended = true;

  // Because canRecordExtended is a per-process variable, we need to make sure
  // that all of the pages load in the same content process. Limit the number
  // of content processes to at most 1 (or 0 if e10s is off entirely).
  await SpecialPowers.pushPrefEnv({ set: [[ "dom.ipc.processCount", 1 ]] });

  gOldContentCanRecord = await ContentTask.spawn(gBrowser.selectedBrowser, {}, function () {
    let telemetry = Cc["@mozilla.org/base/telemetry;1"].getService(Ci.nsITelemetry);
    let old = telemetry.canRecordExtended;
    telemetry.canRecordExtended = true;
    return old;
  });
  info("canRecord for content: " + gOldContentCanRecord);
});

add_task(async function() {
  // Check that use counters are incremented by SVGs loaded directly in iframes.
  await check_use_counter_iframe("file_use_counter_svg_getElementById.svg",
                                 "SVGSVGELEMENT_GETELEMENTBYID");
  await check_use_counter_iframe("file_use_counter_svg_currentScale.svg",
                                 "SVGSVGELEMENT_CURRENTSCALE_getter");
  await check_use_counter_iframe("file_use_counter_svg_currentScale.svg",
                                 "SVGSVGELEMENT_CURRENTSCALE_setter");

  // Check that even loads from the imglib cache update use counters.  The
  // images should still be there, because we just loaded them in the last
  // set of tests.  But we won't get updated counts for the document
  // counters, because we won't be re-parsing the SVG documents.
  await check_use_counter_iframe("file_use_counter_svg_getElementById.svg",
                                 "SVGSVGELEMENT_GETELEMENTBYID", false);
  await check_use_counter_iframe("file_use_counter_svg_currentScale.svg",
                                 "SVGSVGELEMENT_CURRENTSCALE_getter", false);
  await check_use_counter_iframe("file_use_counter_svg_currentScale.svg",
                                 "SVGSVGELEMENT_CURRENTSCALE_setter", false);

  // Check that use counters are incremented by SVGs loaded as images.
  // Note that SVG images are not permitted to execute script, so we can only
  // check for properties here.
  await check_use_counter_img("file_use_counter_svg_getElementById.svg",
                              "PROPERTY_FILL");
  await check_use_counter_img("file_use_counter_svg_currentScale.svg",
                              "PROPERTY_FILL");

  // Check that use counters are incremented by directly loading SVGs
  // that reference patterns defined in another SVG file.
  await check_use_counter_direct("file_use_counter_svg_fill_pattern.svg",
                                 "PROPERTY_FILLOPACITY", /*xfail=*/true);

  // Check that use counters are incremented by directly loading SVGs
  // that reference patterns defined in the same file or in data: URLs.
  await check_use_counter_direct("file_use_counter_svg_fill_pattern_internal.svg",
                                 "PROPERTY_FILLOPACITY");
  // data: URLs don't correctly propagate to their referring document yet.
  //yield check_use_counter_direct("file_use_counter_svg_fill_pattern_data.svg",
  //                               "PROPERTY_FILL_OPACITY");
});

add_task(async function() {
  let Telemetry = Cc["@mozilla.org/base/telemetry;1"].getService(Ci.nsITelemetry);
  Telemetry.canRecordExtended = gOldParentCanRecord;

  await ContentTask.spawn(gBrowser.selectedBrowser, { oldCanRecord: gOldContentCanRecord }, async function(arg) {
    ChromeUtils.import("resource://gre/modules/PromiseUtils.jsm");
    await new Promise(resolve => {
      let telemetry = Cc["@mozilla.org/base/telemetry;1"].getService(Ci.nsITelemetry);
      telemetry.canRecordExtended = arg.oldCanRecord;
      resolve();
    });
  });
});


function waitForDestroyedDocuments() {
  let deferred = PromiseUtils.defer();
  SpecialPowers.exactGC(deferred.resolve);
  return deferred.promise;
}

function waitForPageLoad(browser) {
  return ContentTask.spawn(browser, null, async function() {
    ChromeUtils.import("resource://gre/modules/PromiseUtils.jsm");
    await new Promise(resolve => {
      let listener = () => {
        removeEventListener("load", listener, true);
        resolve();
      }
      addEventListener("load", listener, true);
    });
  });
}

function grabHistogramsFromContent(use_counter_middlefix, page_before = null) {
  let telemetry = Cc["@mozilla.org/base/telemetry;1"].getService(Ci.nsITelemetry);
  let gather = () => {
    let snapshots;
    if (Services.appinfo.browserTabsRemoteAutostart) {
      snapshots = telemetry.snapshotHistograms(Ci.nsITelemetry.DATASET_RELEASE_CHANNEL_OPTIN, false).content;
    } else {
      snapshots = telemetry.snapshotHistograms(Ci.nsITelemetry.DATASET_RELEASE_CHANNEL_OPTIN, false).parent;
    }
    let checkGet = (probe) => {
      return snapshots[probe] ? snapshots[probe].sum : 0;
    };
    return [
      checkGet("USE_COUNTER2_" + use_counter_middlefix + "_PAGE"),
      checkGet("USE_COUNTER2_" + use_counter_middlefix + "_DOCUMENT"),
      checkGet("CONTENT_DOCUMENTS_DESTROYED"),
      checkGet("TOP_LEVEL_CONTENT_DOCUMENTS_DESTROYED"),
    ];
  };
  return BrowserTestUtils.waitForCondition(() => {
    return page_before != gather()[0];
  }).then(gather, gather);
}

var check_use_counter_iframe = async function(file, use_counter_middlefix, check_documents=true) {
  info("checking " + file + " with histogram " + use_counter_middlefix);

  let newTab = BrowserTestUtils.addTab(gBrowser,  "about:blank");
  gBrowser.selectedTab = newTab;
  newTab.linkedBrowser.stop();

  // Hold on to the current values of the telemetry histograms we're
  // interested in.
  let [histogram_page_before, histogram_document_before,
       histogram_docs_before, histogram_toplevel_docs_before] =
      await grabHistogramsFromContent(use_counter_middlefix);

  gBrowser.selectedBrowser.loadURI(gHttpTestRoot + "file_use_counter_outer.html");
  await waitForPageLoad(gBrowser.selectedBrowser);

  // Inject our desired file into the iframe of the newly-loaded page.
  await ContentTask.spawn(gBrowser.selectedBrowser, { file: file }, function(opts) {
    ChromeUtils.import("resource://gre/modules/PromiseUtils.jsm");
    let deferred = PromiseUtils.defer();

    let wu = content.window.QueryInterface(Ci.nsIInterfaceRequestor).getInterface(Ci.nsIDOMWindowUtils);

    let iframe = content.document.getElementById('content');
    iframe.src = opts.file;
    let listener = (event) => {
      event.target.removeEventListener("load", listener, true);

      // We flush the main document first, then the iframe's document to
      // ensure any propagation that might happen from content->parent should
      // have already happened when counters are reported to telemetry.
      wu.forceUseCounterFlush(content.document);
      wu.forceUseCounterFlush(iframe.contentDocument);

      deferred.resolve();
    };
    iframe.addEventListener("load", listener, true);

    return deferred.promise;
  });
  
  // Tear down the page.
  gBrowser.removeTab(newTab);

  // The histograms only get recorded when the document actually gets
  // destroyed, which might not have happened yet due to GC/CC effects, etc.
  // Try to force document destruction.
  await waitForDestroyedDocuments();

  // Grab histograms again and compare.
  let [histogram_page_after, histogram_document_after,
       histogram_docs_after, histogram_toplevel_docs_after] =
      await grabHistogramsFromContent(use_counter_middlefix, histogram_page_before);

  is(histogram_page_after, histogram_page_before + 1,
     "page counts for " + use_counter_middlefix + " after are correct");
  ok(histogram_toplevel_docs_after >= histogram_toplevel_docs_before + 1,
     "top level document counts are correct");
  if (check_documents) {
    is(histogram_document_after, histogram_document_before + 1,
       "document counts for " + use_counter_middlefix + " after are correct");
  }
};

var check_use_counter_img = async function(file, use_counter_middlefix) {
  info("checking " + file + " as image with histogram " + use_counter_middlefix);

  let newTab = BrowserTestUtils.addTab(gBrowser, "about:blank");
  gBrowser.selectedTab = newTab;
  newTab.linkedBrowser.stop();

  // Hold on to the current values of the telemetry histograms we're
  // interested in.
  let [histogram_page_before, histogram_document_before,
       histogram_docs_before, histogram_toplevel_docs_before] =
      await grabHistogramsFromContent(use_counter_middlefix);

  gBrowser.selectedBrowser.loadURI(gHttpTestRoot + "file_use_counter_outer.html");
  await waitForPageLoad(gBrowser.selectedBrowser);

  // Inject our desired file into the img of the newly-loaded page.
  await ContentTask.spawn(gBrowser.selectedBrowser, { file: file }, async function(opts) {
    ChromeUtils.import("resource://gre/modules/PromiseUtils.jsm");
    let deferred = PromiseUtils.defer();

    let img = content.document.getElementById('display');
    img.src = opts.file;
    let listener = (event) => {
      img.removeEventListener("load", listener, true);

      // Flush for the image.  It matters what order we do these in, so that
      // the image can propagate its use counters to the document prior to the
      // document reporting its use counters.
      let wu = content.window.QueryInterface(Ci.nsIInterfaceRequestor).getInterface(Ci.nsIDOMWindowUtils);
      wu.forceUseCounterFlush(img);

      // Flush for the main window.
      wu.forceUseCounterFlush(content.document);

      deferred.resolve();
    };
    img.addEventListener("load", listener, true);

    return deferred.promise;
  });
  
  // Tear down the page.
  gBrowser.removeTab(newTab);

  // The histograms only get recorded when the document actually gets
  // destroyed, which might not have happened yet due to GC/CC effects, etc.
  // Try to force document destruction.
  await waitForDestroyedDocuments();

  // Grab histograms again and compare.
  let [histogram_page_after, histogram_document_after,
       histogram_docs_after, histogram_toplevel_docs_after] =
      await grabHistogramsFromContent(use_counter_middlefix, histogram_page_before);
  is(histogram_page_after, histogram_page_before + 1,
     "page counts for " + use_counter_middlefix + " after are correct");
  is(histogram_document_after, histogram_document_before + 1,
     "document counts for " + use_counter_middlefix + " after are correct");
  ok(histogram_toplevel_docs_after >= histogram_toplevel_docs_before + 1,
     "top level document counts are correct");
  // 2 documents: one for the outer html page containing the <img> element, and
  // one for the SVG image itself.
  ok(histogram_docs_after >= histogram_docs_before + 2,
     "document counts are correct");
};

var check_use_counter_direct = async function(file, use_counter_middlefix, xfail=false) {
  info("checking " + file + " with histogram " + use_counter_middlefix);

  let newTab = BrowserTestUtils.addTab(gBrowser,  "about:blank");
  gBrowser.selectedTab = newTab;
  newTab.linkedBrowser.stop();

  // Hold on to the current values of the telemetry histograms we're
  // interested in.
  let [histogram_page_before, histogram_document_before,
       histogram_docs_before, histogram_toplevel_docs_before] =
      await grabHistogramsFromContent(use_counter_middlefix);

  gBrowser.selectedBrowser.loadURI(gHttpTestRoot + file);
  await ContentTask.spawn(gBrowser.selectedBrowser, null, async function() {
    ChromeUtils.import("resource://gre/modules/PromiseUtils.jsm");
    await new Promise(resolve => {
      let listener = () => {
        removeEventListener("load", listener, true);

        let wu = content.window.QueryInterface(Ci.nsIInterfaceRequestor).getInterface(Ci.nsIDOMWindowUtils);
        wu.forceUseCounterFlush(content.document);

        setTimeout(resolve, 0);
      }
      addEventListener("load", listener, true);
    });
  });
  
  // Tear down the page.
  gBrowser.removeTab(newTab);

  // The histograms only get recorded when the document actually gets
  // destroyed, which might not have happened yet due to GC/CC effects, etc.
  // Try to force document destruction.
  await waitForDestroyedDocuments();

  // Grab histograms again and compare.
  let [histogram_page_after, histogram_document_after,
       histogram_docs_after, histogram_toplevel_docs_after] =
      await grabHistogramsFromContent(use_counter_middlefix, histogram_page_before);
  if (!xfail) {
    is(histogram_page_after, histogram_page_before + 1,
       "page counts for " + use_counter_middlefix + " after are correct");
    is(histogram_document_after, histogram_document_before + 1,
       "document counts for " + use_counter_middlefix + " after are correct");
  }
  ok(histogram_toplevel_docs_after >= histogram_toplevel_docs_before + 1,
     "top level document counts are correct");
  ok(histogram_docs_after >= histogram_docs_before + 1,
     "document counts are correct");
};
