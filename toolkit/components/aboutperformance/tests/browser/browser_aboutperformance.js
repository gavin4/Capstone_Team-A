/* Any copyright is dedicated to the Public Domain.
 * http://creativecommons.org/publicdomain/zero/1.0/ */

/* eslint-env mozilla/frame-script */
/* eslint-disable mozilla/no-arbitrary-setTimeout */

"use strict";

ChromeUtils.import("resource://testing-common/ContentTask.jsm", this);

const URL = "http://example.com/browser/toolkit/components/aboutperformance/tests/browser/browser_compartments.html?test=" + Math.random();

// This function is injected as source as a frameScript
function frameScript() {
  "use strict";

  addMessageListener("aboutperformance-test:done", () => {
    content.postMessage("stop", "*");
    sendAsyncMessage("aboutperformance-test:done", null);
  });
  addMessageListener("aboutperformance-test:setTitle", ({data: title}) => {
    content.document.title = title;
    sendAsyncMessage("aboutperformance-test:setTitle", null);
  });

  addMessageListener("aboutperformance-test:closeTab", ({data: options}) => {
    let observer = function(subject, topic, mode) {
      dump(`aboutperformance-test:closeTab 1 ${options.url}\n`);
      Services.obs.removeObserver(observer, "about:performance-update-complete");

      let exn;
      let found = false;
      try {
        for (let eltContent of content.document.querySelectorAll("li.delta")) {
          let eltName = eltContent.querySelector("li.name");
          if (!eltName.textContent.includes(options.url)) {
            continue;
          }

          found = true;
          let [eltCloseTab, eltReloadTab] = eltContent.querySelectorAll("button");
          let button;
          if (options.mode == "reload") {
            button = eltReloadTab;
          } else if (options.mode == "close") {
            button = eltCloseTab;
          } else {
            throw new TypeError(options.mode);
          }
          dump(`aboutperformance-test:closeTab clicking on ${button.textContent}\n`);
          button.click();
          return;
        }
      } catch (ex) {
        dump(`aboutperformance-test:closeTab: error ${ex}\n`);
        exn = ex;
      } finally {
        if (exn) {
          sendAsyncMessage("aboutperformance-test:closeTab", { error: {message: exn.message, lineNumber: exn.lineNumber, fileName: exn.fileName}, found});
        } else {
          sendAsyncMessage("aboutperformance-test:closeTab", { ok: true, found });
        }
      }
    };
    Services.obs.addObserver(observer, "about:performance-update-complete");
    Services.obs.notifyObservers(null, "test-about:performance-test-driver", JSON.stringify(options));
  });

  addMessageListener("aboutperformance-test:checkSanity", ({data: options}) => {
    let exn = null;
    try {
      let reFullname = /Full name: (.+)/;
      let reFps = /Impact on framerate: ((\d+) high-impacts, (\d+) medium-impact|(\d+)\/10)?/;
      let reCPU = /CPU usage: (\d+)%/;
      let reCpow = /Blocking process calls: (\d+)%( \((\d+) alerts\))?/;

      let getContentOfSelector = function(eltContainer, selector, re) {
        let elt = eltContainer.querySelector(selector);
        if (!elt) {
          throw new Error(`No item ${selector}`);
        }

        if (!re) {
          return undefined;
        }

        let match = elt.textContent.match(re);
        if (!match) {
          throw new Error(`Item ${selector} doesn't match regexp ${re}: ${elt.textContent}`);
        }
        return match;
      };

      // Additional sanity check
      let deltas = content.document.querySelectorAll(".delta");
      if (!deltas.length) {
        throw new Error("No deltas found to check!");
      }

      for (let eltContent of deltas) {
        // Do we have an attribute "impact"? Is it a number between 0 and 10?
        let impact = eltContent.getAttribute("impact");
        let value = Number.parseInt(impact);
        if (isNaN(value) || value < 0 || value > 10) {
          throw new Error(`Incorrect value ${value}`);
        }

        // Do we have a button "more"?
        getContentOfSelector(eltContent, "a.more");

        // Do we have details?
        getContentOfSelector(eltContent, "ul.details");

        // Do we have a full name? Does it make sense?
        getContentOfSelector(eltContent, "li.name", reFullname);

        let eltDetails = eltContent.querySelector("ul.details");

        // Do we have an impact on framerate? Does it make sense?
        if (!eltDetails.querySelector("li.fps").textContent.includes("no impact")) {
          let [, jankStr,, alertsStr] = getContentOfSelector(eltDetails, "li.fps", reFps);
          let jank = Number.parseInt(jankStr);
          if (jank < 0 || jank > 10 || isNaN(jank)) {
            throw new Error(`Invalid jank ${jankStr}`);
          }
          if (alertsStr) {
            let alerts = Number.parseInt(alertsStr);
            if (alerts < 0 || isNaN(alerts)) {
              throw new Error(`Invalid alerts ${alertsStr}`);
            }
          }
        }

        // Do we have a CPU usage? Does it make sense?
        let [, cpuStr] = getContentOfSelector(eltDetails, "li.cpu", reCPU);
        let cpu = Number.parseInt(cpuStr);
        if (cpu < 0 || isNaN(cpu)) { // Note that cpu can be > 100%.
          throw new Error(`Invalid CPU ${cpuStr}`);
        }

        // Do we have CPOW? Does it make sense?
        let [, cpowStr,, alertsStr2] = getContentOfSelector(eltDetails, "li.cpow", reCpow);
        let cpow = Number.parseInt(cpowStr);
        if (cpow < 0 || isNaN(cpow)) {
          throw new Error(`Invalid cpow ${cpowStr}`);
        }
        if (alertsStr2) {
          let alerts = Number.parseInt(alertsStr2);
          if (alerts < 0 || isNaN(alerts)) {
            throw new Error(`Invalid alerts ${alertsStr2}`);
          }
        }
      }
    } catch (ex) {
      dump(`aboutperformance-test:checkSanity: error ${ex}\n`);
      exn = ex;
    }
    if (exn) {
      sendAsyncMessage("aboutperformance-test:checkSanity", { error: {message: exn.message, lineNumber: exn.lineNumber, fileName: exn.fileName}});
    } else {
      sendAsyncMessage("aboutperformance-test:checkSanity", { ok: true });
    }
  });

  addMessageListener("aboutperformance-test:hasItems", ({data: {title, options}}) => {
    let observer = function(subject, topic, mode) {
      Services.obs.removeObserver(observer, "about:performance-update-complete");
      let hasTitleInWebpages = false;

      try {
        let eltWeb = content.document.getElementById("webpages");
        if (!eltWeb) {
          dump(`aboutperformance-test:hasItems: the page is not ready yet webpages:${eltWeb}\n`);
          return;
        }

        let webTitles = Array.from(eltWeb.querySelectorAll("span.title"), elt => elt.textContent);

        hasTitleInWebpages = webTitles.includes(title);
      } catch (ex) {
        Cu.reportError("Error in content: " + ex);
        Cu.reportError(ex.stack);
      } finally {
        sendAsyncMessage("aboutperformance-test:hasItems", {hasTitleInWebpages, mode});
      }
    };
    Services.obs.addObserver(observer, "about:performance-update-complete");
    Services.obs.notifyObservers(null, "test-about:performance-test-driver", JSON.stringify(options));
  });
}

var gTabAboutPerformance = null;
var gTabContent = null;

add_task(async function init() {
  info("Setting up about:performance");
  gTabAboutPerformance = gBrowser.selectedTab = BrowserTestUtils.addTab(gBrowser, "about:performance");
  await ContentTask.spawn(gTabAboutPerformance.linkedBrowser, null, frameScript);

  info(`Setting up ${URL}`);
  gTabContent = BrowserTestUtils.addTab(gBrowser, URL);
  await ContentTask.spawn(gTabContent.linkedBrowser, null, frameScript);
});

var promiseExpectContent = async function(options) {
  let title = "Testing about:performance " + Math.random();
  for (let i = 0; i < 30; ++i) {
    await new Promise(resolve => setTimeout(resolve, 100));
    await promiseContentResponse(gTabContent.linkedBrowser, "aboutperformance-test:setTitle", title);
    let {hasTitleInWebpages, mode} = (await promiseContentResponse(gTabAboutPerformance.linkedBrowser, "aboutperformance-test:hasItems", {title, options}));

    info(`aboutperformance-test:hasItems ${hasTitleInWebpages}, ${mode}, ${options.displayRecent}`);
    if (!hasTitleInWebpages) {
      info(`Title not found in webpages`);
      continue;
    }
    if ((mode == "recent") != options.displayRecent) {
      info(`Wrong mode`);
      continue;
    }

    let { ok, error } = await promiseContentResponse(gTabAboutPerformance.linkedBrowser, "aboutperformance-test:checkSanity", {options});
    if (ok) {
      info("aboutperformance-test:checkSanity: success");
    }
    if (error) {
      Assert.ok(false, `aboutperformance-test:checkSanity error: ${JSON.stringify(error)}`);
    }
    return true;
  }
  return false;
};

// Test that we can find the title of a webpage in about:performance
add_task(async function test_find_title() {
    for (let displayRecent of [true, false]) {
      info(`Testing with autoRefresh, in ${displayRecent ? "recent" : "global"} mode`);
      let found = await promiseExpectContent({autoRefresh: 100, displayRecent});
      Assert.ok(found, `The page title appears when about:performance is set to auto-refresh`);
    }
});

// Test that we can close/reload tabs using the corresponding buttons
add_task(async function test_close_tab() {
  let tabs = new Map();
  let closeObserver = function({type, originalTarget: tab}) {
    dump(`closeObserver: ${tab}, ${tab.constructor.name}, ${tab.tagName}, ${type}\n`);
    let cb = tabs.get(tab);
    if (cb) {
      cb(type);
    }
  };
  let promiseTabClosed = function(tab) {
    return new Promise(resolve => tabs.set(tab, resolve));
  };
  window.gBrowser.tabContainer.addEventListener("TabClose", closeObserver);
  let promiseTabReloaded = function(tab) {
    return new Promise(resolve =>
      tab.linkedBrowser.contentDocument.addEventListener("readystatechange", resolve)
    );
  };
  for (let displayRecent of [true, false]) {
    for (let mode of ["close", "reload"]) {
      let URL = `about:about?display-recent=${displayRecent}&mode=${mode}&salt=${Math.random()}`;
      info(`Setting up ${URL}`);
      let tab = BrowserTestUtils.addTab(gBrowser, URL);
      await ContentTask.spawn(tab.linkedBrowser, null, frameScript);
      let promiseClosed = promiseTabClosed(tab);
      let promiseReloaded = promiseTabReloaded(tab);

      info(`Requesting close`);
      do {
        await new Promise(resolve => setTimeout(resolve, 100));
        await promiseContentResponse(tab.linkedBrowser, "aboutperformance-test:setTitle", URL);

        let {ok, found, error} = await promiseContentResponse(gTabAboutPerformance.linkedBrowser, "aboutperformance-test:closeTab", {url: URL, autoRefresh: true, mode, displayRecent});
        Assert.ok(ok, `Message aboutperformance-test:closeTab was handled correctly ${JSON.stringify(error)}`);
        info(`URL ${URL} ${found ? "found" : "hasn't been found yet"}`);
        if (found) {
          break;
        }
      } while (true);

      if (mode == "close") {
        info(`Waiting for close`);
        await promiseClosed;
      } else {
        info(`Waiting for reload`);
        await promiseReloaded;
        BrowserTestUtils.removeTab(tab);
      }
    }
  }
});

add_task(async function cleanup() {
  // Cleanup
  info("Cleaning up");
  await promiseContentResponse(gTabAboutPerformance.linkedBrowser, "aboutperformance-test:done", null);

  info("Closing tabs");
  for (let tab of gBrowser.tabs) {
    BrowserTestUtils.removeTab(tab);
  }

  info("Done");
  gBrowser.selectedTab = null;
});
