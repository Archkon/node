'use strict';

require('../common');

const assert = require('assert');
const { spawnSync } = require('child_process');
const fixtures = require('../common/fixtures');

const fixture = fixtures.path('http-client-early-response-reset.js');
const { error, signal, status, stdout, stderr } = spawnSync(
  process.execPath,
  [fixture],
  { encoding: 'utf8' },
);

assert.ifError(error);
assert.strictEqual(signal, null, stderr);
assert.strictEqual(status, 0, stderr);
assert.match(stdout, /(?:^|\n)413\n$/);
