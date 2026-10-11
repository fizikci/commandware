const assert = require('node:assert/strict');
const fs = require('node:fs');
const vm = require('node:vm');
const path = require('node:path');
const tests = [];
function test(name, run) { tests.push({name, run}); }

const sketch = fs.readFileSync(path.join(__dirname, '../firmware/esp12e_commandware/esp12e_commandware.ino'), 'utf8');
const transport = sketch.slice(sketch.indexOf('let requestQueue='), sketch.indexOf('async function refresh()'));
const response = text => ({ok: true, text: async () => text});
function client(fetch) {
  const timers = new Map();
  let id = 0;
  const context = vm.createContext({fetch, URLSearchParams, AbortController,
    setTimeout: (callback, delay) => { assert.equal(delay, 10000); timers.set(++id, callback); return id; },
    clearTimeout: key => timers.delete(key)});
  vm.runInContext(transport, context);
  return {send: context.send, timers};
}

test('commands and jobs polling share one request at a time', async () => {
  const calls = [];
  let completeFirst;
  const c = client(async (_, options) => {
    calls.push(options.body.get('command'));
    if (calls.length === 1) return new Promise(resolve => {completeFirst = resolve});
    return response('second');
  });
  const first = c.send('ohm A0'), second = c.send('jobs');
  await Promise.resolve();
  assert.deepEqual(calls, ['ohm A0']);
  completeFirst(response('9878'));
  assert.equal(await first, '9878');
  assert.equal(await second, 'second');
  assert.deepEqual(calls, ['ohm A0', 'jobs']);
  assert.equal(c.timers.size, 0);
});

test('timeout releases the queue without retrying a command', async () => {
  let calls = 0;
  const c = client(async (_, options) => {
    if (++calls > 1) return response('ready');
    return new Promise((_, reject) => options.signal.addEventListener('abort', () => {
      reject(Object.assign(new Error('aborted'), {name: 'AbortError'}));
    }));
  });
  const first = c.send('write 4 1');
  const rejected = assert.rejects(first, /timed out.*may have executed/);
  const second = c.send('status');
  await Promise.resolve();
  [...c.timers.values()][0]();
  await rejected;
  assert.equal(await second, 'ready');
  assert.equal(calls, 2);
  assert.equal(c.timers.size, 0);
});

test('HTTP errors release the queue and preserve device error text', async () => {
  let calls = 0;
  const c = client(async () => ++calls === 1
    ? {ok: false, text: async () => 'ERROR: bad command'} : response('ready'));
  await assert.rejects(c.send('bad'), /ERROR: bad command/);
  assert.equal(await c.send('status'), 'ready');
  assert.equal(c.timers.size, 0);
});

test('multiline schedule reaches the device as one command', async () => {
  const command = 'schedule {\n 1000 {clock 1000 --dur 500 | write 15}\n 1500 {clock 1400 --dur 1000 | write 15}\n}';
  let calls = 0;
  const c = client(async (_, options) => {
    ++calls;
    assert.equal(options.body.get('command'), command);
    assert.equal(new URLSearchParams(options.body.toString()).get('command'), command);
    return response('Scheduled job 1\nScheduled job 2');
  });
  assert.match(await c.send(command), /Scheduled job 2/);
  assert.equal(calls, 1);
});

// Run with `node tests/web_request.test.cjs` (Node 16+).
(async () => {
  for (const {name, run} of tests) {
    await run();
    console.log(`PASS ${name}`);
  }
})().catch(error => {console.error(error); process.exitCode = 1;});
