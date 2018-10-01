// ----------------------------------------------------------------------------
// Test that an install that requires cookies to be sent succeeds when cookies
// are set and third party cookies are disabled.
// This verifies bug 462739
function test() {
  Harness.installEndedCallback = install_ended;
  Harness.installsCompletedCallback = finish_test;
  Harness.setup();

  Services.cookies.add("example.com", "/browser/" + RELATIVE_DIR, "xpinstall",
    "true", false, false, true, (Date.now() / 1000) + 60, {},
    Ci.nsICookie2.SAMESITE_UNSET);

  var pm = Services.perms;
  pm.add(makeURI("http://example.com/"), "install", pm.ALLOW_ACTION);

  Services.prefs.setIntPref("network.cookie.cookieBehavior", 1);

  var triggers = encodeURIComponent(JSON.stringify({
    "Cookie check": TESTROOT + "cookieRedirect.sjs?" + TESTROOT + "amosigned.xpi",
  }));
  gBrowser.selectedTab = BrowserTestUtils.addTab(gBrowser);
  BrowserTestUtils.loadURI(gBrowser, TESTROOT + "installtrigger.html?" + triggers);
}

function install_ended(install, addon) {
  install.cancel();
}

function finish_test(count) {
  is(count, 1, "1 Add-on should have been successfully installed");

  Services.cookies.remove("example.com", "xpinstall", "/browser/" + RELATIVE_DIR,
    false, {});

  Services.prefs.clearUserPref("network.cookie.cookieBehavior");

  Services.perms.remove(makeURI("http://example.com"), "install");

  gBrowser.removeCurrentTab();
  Harness.finish();
}
