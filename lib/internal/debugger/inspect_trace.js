'use strict';

const {
  ArrayPrototypePush,
  DateNow,
  JSONStringify,
  NumberParseInt,
  ObjectCreate,
  RegExpPrototypeSymbolReplace,
  String,
  StringPrototypeSlice,
} = primordials;

const fs = require('fs');
const path = require('path');

const enabled = process.env.NODE_INSPECT_TRACE === '1';
const traces = [];
let signalHandlersInstalled = false;
let dumpCounter = 0;

function parseLimit() {
  const value = process.env.NODE_INSPECT_TRACE_LIMIT;
  if (value === undefined) return 256;
  const parsed = NumberParseInt(value, 10);
  return parsed > 0 ? parsed : 256;
}

const defaultLimit = parseLimit();

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

function snapshotTrace(trace) {
  const out = [];
  const { buffer, cursor, length, limit } = trace;
  for (let i = 0; i < length; i++) {
    const index = length === limit ? (cursor + i) % limit : i;
    ArrayPrototypePush(out, buffer[index]);
  }
  return out;
}

function snapshotAll() {
  const out = ObjectCreate(null);
  const counts = ObjectCreate(null);
  for (let i = 0; i < traces.length; i++) {
    const trace = traces[i];
    const count = counts[trace.section] ?? 0;
    counts[trace.section] = count + 1;
    const section = count === 0 ? trace.section : `${trace.section}.${count}`;
    out[section] = snapshotTrace(trace);
  }
  return out;
}

function dumpAll(reason) {
  if (!enabled) return;
  const dir = process.env.NODE_INSPECT_DEBUG_LOG_DIR;
  if (!dir) return;

  try {
    fs.mkdirSync(dir, { recursive: true });
    const file = path.join(
      dir,
      `${DateNow()}-${process.pid}-${++dumpCounter}-${sanitizeName(reason)}-inspect-trace.json`,
    );
    fs.writeFileSync(file, `${JSONStringify({
      reason,
      pid: process.pid,
      trace: snapshotAll(),
    }, null, 2)}\n`);
    return file;
  } catch {
    // Tracing must never affect debugger behavior.
  }
}

function installSignalHandlers() {
  if (!enabled ||
      signalHandlersInstalled ||
      process.env.NODE_INSPECT_TRACE_DUMP_ON_SIGNAL !== '1') {
    return;
  }
  signalHandlersInstalled = true;

  process.once('SIGTERM', () => {
    dumpAll('SIGTERM');
    // Preserve the default SIGTERM behavior when tracing is the only listener.
    if (process.listenerCount('SIGTERM') === 0) {
      process.kill(process.pid, 'SIGTERM');
    }
  });
}

class RingTrace {
  constructor(section, limit = defaultLimit) {
    this.section = sanitizeName(section);
    this.limit = limit;
    this.buffer = [];
    this.cursor = 0;
    this.length = 0;
    if (enabled) {
      ArrayPrototypePush(traces, this);
      installSignalHandlers();
    }
  }

  record(event, data) {
    if (!enabled) return;
    const entry = data === undefined ?
      { t: DateNow(), event } :
      { t: DateNow(), event, data };
    if (this.length < this.limit) {
      ArrayPrototypePush(this.buffer, entry);
      this.length++;
      return;
    }
    this.buffer[this.cursor] = entry;
    this.cursor = (this.cursor + 1) % this.limit;
  }

  snapshot() {
    if (!enabled) return [];
    return snapshotTrace(this);
  }
}

function createTrace(section, limit) {
  return new RingTrace(section, limit);
}

module.exports = {
  createTrace,
  dumpAll,
  snapshotAll,
};
