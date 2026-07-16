'use strict';

const common = require('../common');
const assert = require('assert');
const http = require('http');

const BODY = Buffer.alloc(2 * 1024 * 1024);

const server = http.createServer(common.mustCall((request, response) => {
  let received = 0;

  request.on('data', function onData(chunk) {
    received += chunk.length;
    if (received > BODY.length / 2) {
      request.off('data', onData);
      response.writeHead(413, { 'content-length': 0 });
      response.end();
      request.destroy();
    }
  });

  request.on('end', common.mustNotCall());
  request.on('close', common.mustCall(() => {
    assert.strictEqual(request.complete, false);
    assert.ok(received > 0);
  }));
}));

server.listen(0, common.mustCall(() => {
  let response;
  const req = http.request({
    method: 'POST',
    port: server.address().port,
    headers: {
      'content-type': 'application/json',
      'content-length': BODY.length,
    },
  }, (res) => {
    response = res;
    assert.strictEqual(res.statusCode, 413);
    // Deliberately do not consume the response body. A response that has
    // completed after the request finished should not be followed by a late
    // ClientRequest socket error.
  });

  let socketError;
  function logState(phase) {
    console.error({
      phase,
      code: socketError?.code,
      syscall: socketError?.syscall,
      socketDestroyed: req.socket?.destroyed,
      socketDestroying: req.socket?.destroying,
      socketReadable: req.socket?.readable,
      readableErrored: req.socket?._readableState.errored?.code,
      readableErrorEmitted: req.socket?._readableState.errorEmitted,
      handleReading: req.socket?._handle?.reading,
      hasParser: Boolean(req.socket?.parser),
      hasResponse: Boolean(req.res),
      writableEnded: req.writableEnded,
      writableFinished: req.writableFinished,
      responseComplete: response?.complete,
      reqResComplete: req.res?.complete,
    });
  }

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

      // A write error marks both sides of the Duplex as errored. Keep the
      // writable error, but allow already-buffered response data to reach the
      // HTTP parser for this diagnostic test.
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
    logState('error');
    setImmediate(() => logState('after-error-immediate'));
  });
  req.on('close', common.mustCall(() => {
    logState('close');
    setTimeout(() => {
      logState('after-error-timeout');
      server.close();
    }, 100);
  }));

  req.end(BODY);
}));
