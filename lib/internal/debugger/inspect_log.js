'use strict';

const {
  DateNow,
  RegExpPrototypeSymbolReplace,
  String,
  StringPrototypeSlice,
} = primordials;

const fs = require('fs');
const path = require('path');
const { format } = require('internal/util/inspect');

let warned = false;

function sanitizeName(value) {
  return StringPrototypeSlice(
    RegExpPrototypeSymbolReplace(
      /^_+|_+$/g,
      RegExpPrototypeSymbolReplace(/[^A-Za-z0-9_.-]+/g, String(value), '_'),
      '',
    ),
    0,
    80,
  ) || 'inspect';
}

function appendDetailedLog(section, line) {
  const dir = process.env.NODE_INSPECT_DEBUG_LOG_DIR;
  if (!dir) return;

  try {
    fs.mkdirSync(dir, { recursive: true });
    fs.appendFileSync(
      path.join(dir, `js-${sanitizeName(section)}-${process.pid}.log`),
      `${DateNow()} ${line}\n`,
    );
  } catch (err) {
    if (!warned) {
      warned = true;
      process.stderr.write(`Failed to write inspect debug log: ${err.message}\n`);
    }
  }
}

function createDetailedLogger(section, debug) {
  if (process.env.NODE_INSPECT_DETAILED_LOG !== '1') {
    return () => {};
  }

  return (...args) => {
    debug(...args);
    appendDetailedLog(section, `[${section}] ${format(...args)}`);
  };
}

module.exports = {
  createDetailedLogger,
};
