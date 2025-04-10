// Enable signature checks for these tests
gUseRealCertChecks = true;
// Disable update security
Services.prefs.setBoolPref(PREF_EM_CHECK_UPDATE_SECURITY, false);

const DATA = "data/signing_checks/";
const ADDONS = {
  bootstrap: {
    unsigned: "unsigned_bootstrap_2.xpi",
    badid: "signed_bootstrap_badid_2.xpi",
    preliminary: "preliminary_bootstrap_2.xpi",
    signed: "signed_bootstrap_2.xpi",
    sha256Signed: "signed_bootstrap_sha256_1.xpi",
    privileged: "privileged_bootstrap_2.xpi",
  },
};
const WORKING = "signed_bootstrap_1.xpi";
const ID = "test@tests.mozilla.org";

var gServer = createHttpServer({port: 4444});

// Creates an add-on with a broken signature by changing an existing file
function createBrokenAddonModify(file) {
  let brokenFile = gTmpD.clone();
  brokenFile.append("broken.xpi");
  file.copyTo(brokenFile.parent, brokenFile.leafName);

  var stream = Cc["@mozilla.org/io/string-input-stream;1"].
               createInstance(Ci.nsIStringInputStream);
  stream.setData("FOOBAR", -1);
  var zipW = Cc["@mozilla.org/zipwriter;1"].
             createInstance(Ci.nsIZipWriter);
  zipW.open(brokenFile, FileUtils.MODE_RDWR | FileUtils.MODE_APPEND);
  zipW.removeEntry("test.txt", false);
  zipW.addEntryStream("test.txt", 0, Ci.nsIZipWriter.COMPRESSION_NONE,
                      stream, false);
  zipW.close();

  return brokenFile;
}

// Creates an add-on with a broken signature by adding a new file
function createBrokenAddonAdd(file) {
  let brokenFile = gTmpD.clone();
  brokenFile.append("broken.xpi");
  file.copyTo(brokenFile.parent, brokenFile.leafName);

  var stream = Cc["@mozilla.org/io/string-input-stream;1"].
               createInstance(Ci.nsIStringInputStream);
  stream.setData("FOOBAR", -1);
  var zipW = Cc["@mozilla.org/zipwriter;1"].
             createInstance(Ci.nsIZipWriter);
  zipW.open(brokenFile, FileUtils.MODE_RDWR | FileUtils.MODE_APPEND);
  zipW.addEntryStream("test2.txt", 0, Ci.nsIZipWriter.COMPRESSION_NONE,
                      stream, false);
  zipW.close();

  return brokenFile;
}

// Creates an add-on with a broken signature by removing an existing file
function createBrokenAddonRemove(file) {
  let brokenFile = gTmpD.clone();
  brokenFile.append("broken.xpi");
  file.copyTo(brokenFile.parent, brokenFile.leafName);

  var stream = Cc["@mozilla.org/io/string-input-stream;1"].
               createInstance(Ci.nsIStringInputStream);
  stream.setData("FOOBAR", -1);
  var zipW = Cc["@mozilla.org/zipwriter;1"].
             createInstance(Ci.nsIZipWriter);
  zipW.open(brokenFile, FileUtils.MODE_RDWR | FileUtils.MODE_APPEND);
  zipW.removeEntry("test.txt", false);
  zipW.close();

  return brokenFile;
}

function createInstall(url) {
  return AddonManager.getInstallForURL(url, "application/x-xpinstall");
}

function serveUpdateRDF(leafName) {
  gServer.registerPathHandler("/update.rdf", function(request, response) {
    response.setStatusLine(request.httpVersion, 200, "OK");
    response.write(JSON.stringify({
      addons: {
        [ID]: {
          updates: [
            {
              version: "2.0",
              update_link: "http://localhost:4444/" + leafName,
              applications: {
                gecko: {
                  strict_min_version: "4",
                  advisory_max_version: "6",
                },
              },
            },
          ],
        },
      },
    }));
  });
}


async function test_install_broken(file, expectedError) {
  gServer.registerFile("/" + file.leafName, file);

  let install = await createInstall("http://localhost:4444/" + file.leafName);
  await promiseCompleteAllInstalls([install]);

  Assert.equal(install.state, AddonManager.STATE_DOWNLOAD_FAILED);
  Assert.equal(install.error, expectedError);
  Assert.equal(install.addon, null);

  gServer.registerFile("/" + file.leafName, null);
}

async function test_install_working(file, expectedSignedState) {
  gServer.registerFile("/" + file.leafName, file);

  let install = await createInstall("http://localhost:4444/" + file.leafName);
  await promiseCompleteAllInstalls([install]);

  Assert.equal(install.state, AddonManager.STATE_INSTALLED);
  Assert.notEqual(install.addon, null);
  Assert.equal(install.addon.signedState, expectedSignedState);

  gServer.registerFile("/" + file.leafName, null);

  install.addon.uninstall();
}

async function test_update_broken(file, expectedError) {
  // First install the older version
  await promiseInstallAllFiles([do_get_file(DATA + WORKING)]);

  gServer.registerFile("/" + file.leafName, file);
  serveUpdateRDF(file.leafName);

  let addon = await promiseAddonByID(ID);
  let update = await promiseFindAddonUpdates(addon);
  let install = update.updateAvailable;
  await promiseCompleteAllInstalls([install]);

  Assert.equal(install.state, AddonManager.STATE_DOWNLOAD_FAILED);
  Assert.equal(install.error, expectedError);
  Assert.equal(install.addon, null);

  gServer.registerFile("/" + file.leafName, null);
  gServer.registerPathHandler("/update.rdf", null);

  addon.uninstall();
}

async function test_update_working(file, expectedSignedState) {
  // First install the older version
  await promiseInstallAllFiles([do_get_file(DATA + WORKING)]);

  gServer.registerFile("/" + file.leafName, file);
  serveUpdateRDF(file.leafName);

  let addon = await promiseAddonByID(ID);
  let update = await promiseFindAddonUpdates(addon);
  let install = update.updateAvailable;
  await promiseCompleteAllInstalls([install]);

  Assert.equal(install.state, AddonManager.STATE_INSTALLED);
  Assert.notEqual(install.addon, null);
  Assert.equal(install.addon.signedState, expectedSignedState);

  gServer.registerFile("/" + file.leafName, null);
  gServer.registerPathHandler("/update.rdf", null);

  install.addon.uninstall();
}

add_task(async function setup() {
  createAppInfo("xpcshell@tests.mozilla.org", "XPCShell", "4", "4");
  await promiseStartupManager();
});

// Try to install a broken add-on
add_task(async function() {
  let file = createBrokenAddonModify(do_get_file(DATA + ADDONS.bootstrap.signed));
  await test_install_broken(file, AddonManager.ERROR_CORRUPT_FILE);
  file.remove(true);
});

add_task(async function() {
  let file = createBrokenAddonAdd(do_get_file(DATA + ADDONS.bootstrap.signed));
  await test_install_broken(file, AddonManager.ERROR_CORRUPT_FILE);
  file.remove(true);
});

add_task(async function() {
  let file = createBrokenAddonRemove(do_get_file(DATA + ADDONS.bootstrap.signed));
  await test_install_broken(file, AddonManager.ERROR_CORRUPT_FILE);
  file.remove(true);
});

// Try to install an add-on with an incorrect ID
add_task(async function() {
  let file = do_get_file(DATA + ADDONS.bootstrap.badid);
  await test_install_broken(file, AddonManager.ERROR_CORRUPT_FILE);
});

// Try to install an unsigned add-on
add_task(async function() {
  let file = do_get_file(DATA + ADDONS.bootstrap.unsigned);
  await test_install_broken(file, AddonManager.ERROR_SIGNEDSTATE_REQUIRED);
});

// Try to install a preliminarily reviewed add-on
add_task(async function() {
  let file = do_get_file(DATA + ADDONS.bootstrap.preliminary);
  await test_install_working(file, AddonManager.SIGNEDSTATE_PRELIMINARY);
});

// Try to install a signed add-on
add_task(async function() {
  let file = do_get_file(DATA + ADDONS.bootstrap.signed);
  await test_install_working(file, AddonManager.SIGNEDSTATE_SIGNED);
});

// Try to install an add-on signed with SHA-256
add_task(async function() {
  let file = do_get_file(DATA + ADDONS.bootstrap.sha256Signed);
  await test_install_working(file, AddonManager.SIGNEDSTATE_SIGNED);
});

// Try to install an add-on with the "Mozilla Extensions" OU
add_task(async function() {
  let file = do_get_file(DATA + ADDONS.bootstrap.privileged);
  await test_install_working(file, AddonManager.SIGNEDSTATE_PRIVILEGED);
});

// Try to update to a broken add-on
add_task(async function() {
  let file = createBrokenAddonModify(do_get_file(DATA + ADDONS.bootstrap.signed));
  await test_update_broken(file, AddonManager.ERROR_CORRUPT_FILE);
  file.remove(true);
});

add_task(async function() {
  let file = createBrokenAddonAdd(do_get_file(DATA + ADDONS.bootstrap.signed));
  await test_update_broken(file, AddonManager.ERROR_CORRUPT_FILE);
  file.remove(true);
});

add_task(async function() {
  let file = createBrokenAddonRemove(do_get_file(DATA + ADDONS.bootstrap.signed));
  await test_update_broken(file, AddonManager.ERROR_CORRUPT_FILE);
  file.remove(true);
});

// Try to update to an add-on with an incorrect ID
add_task(async function() {
  let file = do_get_file(DATA + ADDONS.bootstrap.badid);
  await test_update_broken(file, AddonManager.ERROR_CORRUPT_FILE);
});

// Try to update to an unsigned add-on
add_task(async function() {
  let file = do_get_file(DATA + ADDONS.bootstrap.unsigned);
  await test_update_broken(file, AddonManager.ERROR_SIGNEDSTATE_REQUIRED);
});

// Try to update to a preliminarily reviewed add-on
add_task(async function() {
  let file = do_get_file(DATA + ADDONS.bootstrap.preliminary);
  await test_update_working(file, AddonManager.SIGNEDSTATE_PRELIMINARY);
});

// Try to update to a signed add-on
add_task(async function() {
  let file = do_get_file(DATA + ADDONS.bootstrap.signed);
  await test_update_working(file, AddonManager.SIGNEDSTATE_SIGNED);
});
