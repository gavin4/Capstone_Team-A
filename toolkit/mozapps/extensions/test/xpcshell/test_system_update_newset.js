// Tests that system add-on upgrades work.

ChromeUtils.import("resource://testing-common/httpd.js");

BootstrapMonitor.init();

createAppInfo("xpcshell@tests.mozilla.org", "XPCShell", "2");

var testserver = new HttpServer();
testserver.registerDirectory("/data/", do_get_file("data/system_addons"));
testserver.start();
var root = testserver.identity.primaryScheme + "://" +
           testserver.identity.primaryHost + ":" +
           testserver.identity.primaryPort + "/data/";
Services.prefs.setCharPref(PREF_SYSTEM_ADDON_UPDATE_URL, root + "update.xml");

let distroDir = FileUtils.getDir("ProfD", ["sysfeatures", "empty"], true);
registerDirectory("XREAppFeat", distroDir);
initSystemAddonDirs();

/**
 * Defines the set of initial conditions to run each test against. Each should
 * define the following properties:
 *
 * setup:        A task to setup the profile into the initial state.
 * initialState: The initial expected system add-on state after setup has run.
 */
const TEST_CONDITIONS = {
  // Runs tests with no updated or default system add-ons initially installed
  blank: {
    setup() {
      clearSystemAddonUpdatesDir();
      distroDir.leafName = "empty";
    },
    initialState: [
      { isUpgrade: false, version: null},
      { isUpgrade: false, version: null},
      { isUpgrade: false, version: null},
      { isUpgrade: false, version: null},
      { isUpgrade: false, version: null},
    ],
  },
  // Runs tests with default system add-ons installed
  withAppSet: {
    setup() {
      clearSystemAddonUpdatesDir();
      distroDir.leafName = "prefilled";
    },
    initialState: [
      { isUpgrade: false, version: null},
      { isUpgrade: false, version: "2.0"},
      { isUpgrade: false, version: "2.0"},
      { isUpgrade: false, version: null},
      { isUpgrade: false, version: null},
    ],
  },

  // Runs tests with updated system add-ons installed
  withProfileSet: {
    setup() {
      buildPrefilledUpdatesDir();
      distroDir.leafName = "empty";
    },
    initialState: [
      { isUpgrade: false, version: null},
      { isUpgrade: true, version: "2.0"},
      { isUpgrade: true, version: "2.0"},
      { isUpgrade: false, version: null},
      { isUpgrade: false, version: null},
    ],
  },

  // Runs tests with both default and updated system add-ons installed
  withBothSets: {
    setup() {
      buildPrefilledUpdatesDir();
      distroDir.leafName = "hidden";
    },
    initialState: [
      { isUpgrade: false, version: "1.0"},
      { isUpgrade: true, version: "2.0"},
      { isUpgrade: true, version: "2.0"},
      { isUpgrade: false, version: null},
      { isUpgrade: false, version: null},
    ],
  },
};

/**
 * The tests to run. Each test must define an updateList or test. The following
 * properties are used:
 *
 * updateList: The set of add-ons the server should respond with.
 * test:       A function to run to perform the update check (replaces
 *             updateList)
 * fails:      An optional property, if true the update check is expected to
 *             fail.
 * finalState: An optional property, the expected final state of system add-ons,
 *             if missing the test condition's initialState is used.
 */
const TESTS = {
  // Tests that a new set of system add-ons gets installed
  newset: {
    updateList: [
      { id: "system4@tests.mozilla.org", version: "1.0", path: "system4_1.xpi" },
      { id: "system5@tests.mozilla.org", version: "1.0", path: "system5_1.xpi" },
    ],
    finalState: {
      blank: [
        { isUpgrade: false, version: null},
        { isUpgrade: false, version: null},
        { isUpgrade: false, version: null},
        { isUpgrade: true, version: "1.0"},
        { isUpgrade: true, version: "1.0"},
      ],
      withAppSet: [
        { isUpgrade: false, version: null},
        { isUpgrade: false, version: "2.0"},
        { isUpgrade: false, version: "2.0"},
        { isUpgrade: true, version: "1.0"},
        { isUpgrade: true, version: "1.0"},
      ],
      withProfileSet: [
        { isUpgrade: false, version: null},
        { isUpgrade: false, version: null},
        { isUpgrade: false, version: null},
        { isUpgrade: true, version: "1.0"},
        { isUpgrade: true, version: "1.0"},
      ],
      withBothSets: [
        { isUpgrade: false, version: "1.0"},
        { isUpgrade: false, version: "1.0"},
        { isUpgrade: false, version: null},
        { isUpgrade: true, version: "1.0"},
        { isUpgrade: true, version: "1.0"},
      ],
    },
  },
};

add_task(async function setup() {
  // Initialise the profile
  await overrideBuiltIns({ "system": [] });
  await promiseStartupManager();
  await promiseShutdownManager();
});

add_task(async function() {
  for (let setupName of Object.keys(TEST_CONDITIONS)) {
    for (let testName of Object.keys(TESTS)) {
        info("Running test " + setupName + " " + testName);

        let setup = TEST_CONDITIONS[setupName];
        let test = TESTS[testName];

        await execSystemAddonTest(setupName, setup, test, distroDir, root, testserver);
    }
  }
});
