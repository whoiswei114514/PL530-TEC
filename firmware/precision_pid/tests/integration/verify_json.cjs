const assert = require('node:assert/strict');
const { spawnSync } = require('node:child_process');
const result = spawnSync(process.argv[2], [], { encoding: 'utf8' });
process.stdout.write(result.stdout || '');
process.stderr.write(result.stderr || '');
assert.ifError(result.error);
assert.equal(result.status, 0, 'PID API tests failed');
let count = 0;
for (const line of result.stdout.split(/\r?\n/)) {
  if (!line.startsWith('JSON\t')) continue;
  const json = JSON.parse(line.slice(line.indexOf('\t', 5) + 1));
  assert.equal(typeof json, 'object');
  count++;
}
assert(count > 0, 'No response JSON tested');
console.log(`${count} PID response JSON documents parsed`);
