"use strict";

const UUID_REGEX = /^([0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12})$/;

function testFilePath(container, expectedFilePath) {
  // Verify that the path to the install location is shown next to its label.
  let filePath = container.querySelector(".file-path");
  ok(filePath, "file path is in DOM");
  ok(filePath.textContent.endsWith(expectedFilePath), "file path is set correctly");
  is(filePath.previousElementSibling.textContent, "Location", "file path has label");
}

add_task(async function testLegacyAddon() {
  let addonId = "test-devtools@mozilla.org";
  let addonName = "test-devtools";
  let { tab, document } = await openAboutDebugging("addons");
  await waitForInitialAddonList(document);

  await installAddon({
    document,
    path: "addons/unpacked/install.rdf",
    name: addonName,
  });

  let container = document.querySelector(`[data-addon-id="${addonId}"]`);
  testFilePath(container, "browser/devtools/client/aboutdebugging/test/addons/unpacked/");

  await uninstallAddon({document, id: addonId, name: addonName});

  await closeAboutDebugging(tab);
});

add_task(async function testWebExtension() {
  let addonId = "test-devtools-webextension-nobg@mozilla.org";
  let addonName = "test-devtools-webextension-nobg";
  let { tab, document } = await openAboutDebugging("addons");

  await waitForInitialAddonList(document);
  await installAddon({
    document,
    path: "addons/test-devtools-webextension-nobg/manifest.json",
    name: addonName,
    isWebExtension: true
  });

  let container = document.querySelector(`[data-addon-id="${addonId}"]`);
  testFilePath(container, "/test/addons/test-devtools-webextension-nobg/");

  let extensionID = container.querySelector(".extension-id span");
  ok(extensionID.textContent === "test-devtools-webextension-nobg@mozilla.org");

  let internalUUID = container.querySelector(".internal-uuid span");
  ok(internalUUID.textContent.match(UUID_REGEX), "internalUUID is correct");

  let manifestURL = container.querySelector(".manifest-url");
  ok(manifestURL.href.startsWith("moz-extension://"), "href for manifestURL exists");

  await uninstallAddon({document, id: addonId, name: addonName});

  await closeAboutDebugging(tab);
});

add_task(async function testTemporaryWebExtension() {
  let addonName = "test-devtools-webextension-noid";
  let { tab, document } = await openAboutDebugging("addons");

  await waitForInitialAddonList(document);
  await installAddon({
    document,
    path: "addons/test-devtools-webextension-noid/manifest.json",
    name: addonName,
    isWebExtension: true
  });

  let addons = document.querySelectorAll("#temporary-extensions .addon-target-container");
  // Assuming that our temporary add-on is now at the top.
  let container = addons[addons.length - 1];
  let addonId = container.dataset.addonId;

  let extensionID = container.querySelector(".extension-id span");
  ok(extensionID.textContent.endsWith("@temporary-addon"));

  let temporaryID = container.querySelector(".temporary-id-url");
  ok(temporaryID, "Temporary ID message does appear");

  await uninstallAddon({document, id: addonId, name: addonName});

  await closeAboutDebugging(tab);
});

add_task(async function testUnknownManifestProperty() {
  let addonId = "test-devtools-webextension-unknown-prop@mozilla.org";
  let addonName = "test-devtools-webextension-unknown-prop";
  let { tab, document } = await openAboutDebugging("addons");

  await waitForInitialAddonList(document);
  await installAddon({
    document,
    path: "addons/test-devtools-webextension-unknown-prop/manifest.json",
    name: addonName,
    isWebExtension: true
  });

  info("Wait until the addon appears in about:debugging");
  let container = await waitUntilAddonContainer(addonName, document);

  info("Wait until the installation message appears for the new addon");
  await waitUntilElement(".addon-target-messages", container);

  let messages = container.querySelectorAll(".addon-target-message");
  ok(messages.length === 1, "there is one message");
  ok(messages[0].textContent.match(/Error processing browser_actions/),
     "the message is helpful");
  ok(messages[0].classList.contains("addon-target-warning-message"),
     "the message is a warning");

  await uninstallAddon({document, id: addonId, name: addonName});

  await closeAboutDebugging(tab);
});
