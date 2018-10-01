"use strict";

module.exports = {
  "extends": [
    "plugin:mozilla/browser-test"
  ],

  "rules": {
    "no-unused-vars": ["error", {"args": "none", "varsIgnorePattern": "^end_test$"}],
  }
};
