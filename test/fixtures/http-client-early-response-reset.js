/* eslint-env qunit */
'use script';

const http = require('http');

const BODY_PART_LENGTH = Number(process.argv[2] ?? 512 * 1024);
const MAX_BODY_LENGTH = Number(process.argv[3] ?? 1024 * 1024);

async function request (url, options) {
  return new Promise((resolve, reject) => {
    const req = http.request(url, {
      method: options.method,
      headers: options.headers
    }, (response) => {
      resolve(response);
    });
    // req.on('error', reject); // Adding this makes it work
    req.write(options.body);
    req.end();
  });
}

(async function() {
  const server = http.createServer();
  server.on('request', (request, response) => {
    console.log('server-request', request.url, request.headers);

    let body = '';
    request.setEncoding('utf8');
    request.on('data', function onData (chunk) {
      body += chunk;
      if (body.length > MAX_BODY_LENGTH) {
        // HTTP 413 Payload Too Large
        response.writeHead(413);
        response.end();
        request.off('data', onData);
        request.destroy();
      }
    });
    request.on('end', function onEnd () {
      response.writeHead(202);
      response.end();
    });
  });
  server.on('error', (err) => console.error('server-error', err));
  server.listen(0);
  await new Promise((resolve) => server.on('listening', resolve()));

  const address = `http://localhost:${server.address().port}`;
  const options = {
    method: 'POST',
    headers: {
      'content-type': 'application/json'
    },
    body: JSON.stringify({
      a: 'x'.repeat(BODY_PART_LENGTH),
      b: 'x'.repeat(BODY_PART_LENGTH),
      c: 'x'.repeat(BODY_PART_LENGTH),
      d: 'x'.repeat(BODY_PART_LENGTH)
    }, null, '\t')
  };

  try {
    // Works fine on Node v24.18.0
    // const resp = await fetch(address, options);
    // const data = await resp.text();
    // console.log(resp, data);

    // Works from Node v10.x to v24.15.x and fails in v24.16.x, v24.17.x, v24.18.x
    const resp = await request(address, options);
    console.log(resp.statusCode);
  } catch (e) {
    console.error(e);
  } finally {
    server.close();
  }
}());
