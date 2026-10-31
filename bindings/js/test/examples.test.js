// Runs the example server and client as a consumer would, so the docs cannot go stale.
import assert from 'node:assert/strict';
import { execFile, spawn } from 'node:child_process';
import { once } from 'node:events';
import { fileURLToPath } from 'node:url';
import { test } from 'node:test';
import { promisify } from 'node:util';

const run = promisify(execFile);
const pathOf = (name) => fileURLToPath(new URL(`../example/${name}`, import.meta.url));

test('the example client talks to the example server', { timeout: 20_000 }, async () => {
  const server = spawn('node', [pathOf('server.js'), '0'], { stdio: ['ignore', 'pipe', 'inherit'] });
  try {
    const [line] = await once(server.stdout, 'data');
    const [, port] = /port (\d+)/.exec(String(line));
    const { stdout } = await run('node', [pathOf('client.js'), port]);
    assert.match(stdout, /echo: hello/);
    assert.match(stdout, /greet: HELLO, ALICE/);
  } finally {
    server.kill();
  }
});
