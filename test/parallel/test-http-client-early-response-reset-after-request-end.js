'use strict';

const common = require('../common');
const assert = require('assert');
const http = require('http');

const MAX_BODY_LENGTH = 1024 * 1024;
const BODY = JSON.stringify({
  a: 'x'.repeat(512 * 1024),
  b: 'x'.repeat(512 * 1024),
  c: 'x'.repeat(512 * 1024),
  d: 'x'.repeat(512 * 1024),
}, null, '\t');

const server = http.createServer(common.mustCall((request, response) => {
  let received = 0;

  request.setEncoding('utf8');
  request.on('data', function onData(chunk) {
    received += chunk.length;
    if (received > MAX_BODY_LENGTH) {
      response.writeHead(413, { 'content-length': 0 });
      response.end();
      request.off('data', onData);
      request.destroy();
    }
  });

  request.on('end', common.mustNotCall());
  request.on('close', common.mustCall(() => {
    assert.ok(received > MAX_BODY_LENGTH);
    assert.strictEqual(request.complete, false);
  }));
}));

server.on('clientError', common.mustNotCall());

server.listen(0, common.mustCall(() => {
  let response;
  const req = http.request({
    method: 'POST',
    port: server.address().port,
    headers: {
      'content-type': 'application/json',
    },
  }, common.mustCall((res) => {
    response = res;
    assert.strictEqual(res.statusCode, 413);
    assert.strictEqual(req.writableEnded, true);
    res.resume();
  }));
  req._debugWritableFinished = true;

  let socketError;
  let prefinishEmitted = false;
  let finishEmitted = false;
  function logState(phase) {
    console.error({
      phase,
      code: socketError?.code,
      syscall: socketError?.syscall,
      socketDestroyed: req.socket?.destroyed,
      socketDestroying: req.socket?.destroying,
      socketFd: req.socket?._handle?.fd,
      socketReadable: req.socket?.readable,
      readableErrored: req.socket?._readableState.errored?.code,
      readableErrorEmitted: req.socket?._readableState.errorEmitted,
      handleReading: req.socket?._handle?.reading,
      hasParser: Boolean(req.socket?.parser),
      hasResponse: Boolean(req.res),
      legacyFinished: req.finished,
      writableEnded: req.writableEnded,
      writableFinished: req.writableFinished,
      previousWritableFinished: req.finished &&
        req.outputSize === 0 &&
        (!req.socket || req.socket.writableLength === 0),
      writableLength: req.writableLength,
      outputSize: req.outputSize,
      socketWritableLength: req.socket?.writableLength,
      prefinishEmitted,
      finishEmitted,
      responseComplete: response?.complete,
      reqResComplete: req.res?.complete,
    });
  }

  req.on('prefinish', () => {
    prefinishEmitted = true;
    logState('request-prefinish');
  });
  req.on('finish', () => {
    finishEmitted = true;
    logState('request-finish');
  });

  req.on('socket', (socket) => {
    socket._readableState.autoDestroy = false;
    socket._writableState.autoDestroy = false;

    const httpSocketErrorListener = socket.listeners('error')
      .find((listener) => listener.name === 'socketErrorListener');
    assert(httpSocketErrorListener);
    socket.removeListener('error', httpSocketErrorListener);

    let errorHandlingScheduled = false;
    socket.on('error', (err) => {
      socketError ??= err;
      logState(errorHandlingScheduled ?
        'additional-socket-error' : 'socket-error');

      if (errorHandlingScheduled) return;
      errorHandlingScheduled = true;

      if (!socket.destroyed) {
        socket._readableState.errored = null;
        socket._readableState.errorEmitted = false;
        socket.resume();
        logState('after-readable-recovery');
      }

      setTimeout(() => {
        logState('before-delayed-http-error-listener');
        httpSocketErrorListener.call(socket, err);
        logState('after-delayed-http-error-listener');
      }, 100);
    });
  });

  req.on('error', (err) => {
    socketError = err;
    logState('request-error');
  });
  req.on('error', common.mustNotCall());
  req.on('close', common.mustCall(() => {
    logState('close');
    server.close();
  }));

  // Preserve the ordering from the reported regression: the request body is
  // written and ended before the response is received.
  req.write(BODY);
  req.end();
}));
