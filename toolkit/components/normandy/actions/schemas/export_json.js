#!/usr/bin/env node
/* eslint-env node */

/**
 * This script exports the schemas from this package in JSON format, for use by
 * other tools. It is run as a part of the publishing process to NPM.
 */

const fs = require("fs");
const schemas = require("./index.js");

fs.writeFile("./schemas.json", JSON.stringify(schemas), err => {
  if (err) {
    console.error("error", err);
  }
});
