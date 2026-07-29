'use strict';

const fs = require('fs');
const path = require('path');

let logCounter = 0;

function sanitizeName(value) {
  return String(value)
    .replace(/[^A-Za-z0-9_.-]+/g, '_')
    .replace(/^_+|_+$/g, '')
    .slice(0, 120) || 'inspect-debug';
}

function writeInspectDebugLog(name, content) {
  const dir = process.env.NODE_INSPECT_DEBUG_LOG_DIR;
  if (!dir) return undefined;

  try {
    fs.mkdirSync(dir, { recursive: true });
    const file = path.join(
      dir,
      `${Date.now()}-${process.pid}-${++logCounter}-${sanitizeName(name)}.log`,
    );
    fs.writeFileSync(file, content);
    process.stderr.write(`Wrote inspect debug log to ${file}\n`);
    return file;
  } catch (err) {
    process.stderr.write(`Failed to write inspect debug log: ${err.message}\n`);
  }
}

module.exports = {
  writeInspectDebugLog,
};
