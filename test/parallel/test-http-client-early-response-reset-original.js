'use strict';

require('../common');

const assert = require('assert');
const { spawnSync } = require('child_process');
const fixtures = require('../common/fixtures');

const fixture = fixtures.path('http-client-early-response-reset.js');

function runFixture(...args) {
  const { error, signal, status, stdout, stderr } = spawnSync(
    process.execPath,
    [fixture, ...args.map(String)],
    { encoding: 'utf8' },
  );

  assert.ifError(error);
  assert.strictEqual(signal, null, stderr);
  assert.strictEqual(status, 0, stderr);
  assert.match(stdout, /(?:^|\n)413\n$/);
}

runFixture();
runFixture(32 * 1024, 64 * 1024);
