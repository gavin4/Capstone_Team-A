/* -*- indent-tabs-mode: nil; js-indent-level: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */
"use strict";

ChromeUtils.import("resource://gre/modules/XPCOMUtils.jsm");
ChromeUtils.import("resource://gre/modules/Services.jsm");
ChromeUtils.import("resource://testing-common/httpd.js");

XPCOMUtils.defineLazyServiceGetter(this, "gCaptivePortalDetector",
                                   "@mozilla.org/toolkit/captive-detector;1",
                                   "nsICaptivePortalDetector");

const kCanonicalSitePath = "/canonicalSite.html";
const kCanonicalSiteContent = "true";
const kPrefsCanonicalURL = "captivedetect.canonicalURL";
const kPrefsCanonicalContent = "captivedetect.canonicalContent";
const kPrefsMaxWaitingTime = "captivedetect.maxWaitingTime";
const kPrefsPollingTime = "captivedetect.pollingTime";

var gServer;
var gServerURL;

function setupPrefs() {
  Services.prefs.setCharPref(kPrefsCanonicalURL, gServerURL + kCanonicalSitePath);
  Services.prefs.setCharPref(kPrefsCanonicalContent, kCanonicalSiteContent);
  Services.prefs.setIntPref(kPrefsMaxWaitingTime, 0);
  Services.prefs.setIntPref(kPrefsPollingTime, 1);
}

function run_captivedetect_test(xhr_handler, fakeUIResponse, testfun) {
  gServer = new HttpServer();
  gServer.registerPathHandler(kCanonicalSitePath, xhr_handler);
  gServer.start(-1);
  gServerURL = "http://localhost:" + gServer.identity.primaryPort;

  setupPrefs();

  fakeUIResponse();

  testfun();
}
