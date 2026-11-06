const assert = require('node:assert/strict');
const { spawnSync } = require('node:child_process');

const run = spawnSync(process.argv[2], [], { encoding: 'utf8' });
if (run.error) throw run.error;
const payloads = new Map();
let parsedCount = 0;
for (const line of run.stdout.split(/\r?\n/)) {
  if (!line.startsWith('JSON\t')) {
    if (line) console.log(line);
    continue;
  }
  const [, label, text] = line.split('\t');
  const json = JSON.parse(text);
  payloads.set(label, json);
  ++parsedCount;
}
if (run.stderr) process.stderr.write(run.stderr);
// Validate emitted snapshots even when an independent native regression fails.
if (run.status !== 0) process.exitCode = run.status || 1;
const get = label => {
  assert(payloads.has(label), `Missing JSON fixture: ${label}`);
  return payloads.get(label);
};
for (const label of ['no-sample', 'stale-wrap']) {
  const json = get(label);
  assert.equal(json.temperatureC, null);
  assert.equal(json.rawTemperatureC, null);
  assert.equal(json.sensorOk, false);
}
assert.equal(get('no-sample').sampleAgeMs, null);
assert.equal(get('fresh-wrap').temperatureC, 24.5);
assert.equal(get('fresh-wrap').rawTemperatureC, 24.75);
assert.equal(get('fresh-wrap').sampleAgeMs, 999);
assert.equal(get('fresh-wrap').sensorOk, true);
assert.equal(get('stale-wrap').sampleAgeMs, 1000);
assert.equal(get('nonfinite-temperature').temperatureC, null);
assert.equal(get('nonfinite-temperature').rawTemperatureC, null);
assert.equal(get('escaped-status').wifi.ssid, 'quoted"\\\n\r\t\x01');
assert.equal(get('escaped-status').wifi.error, 'line\nerror');
assert(get('configuration-error').configurationError.length > 0);
assert.equal(get('wifi-saved').wifi.saved, true);
assert.equal(get('wifi-saved').wifi.connected, true);
assert.equal(get('wifi-saved').wifi.connecting, false);
for (const label of ['wifi-save-failure', 'wifi-clear-failure', 'trial-timeout']) {
  const wifi = get(label).wifi;
  assert.equal(wifi.saved, false);
  assert(wifi.error.length > 0);
}
assert.equal(get('wifi-save-failure').wifi.connected, true);
assert.equal(get('wifi-clear-failure').wifi.connected, false);
assert.equal(get('trial-timeout').wifi.retrying, false);
const pendingWifi = get('wifi-old-association-pending').wifi;
assert.equal(pendingWifi.connected, false);
assert.equal(pendingWifi.connecting, true);
assert.equal(pendingWifi.saved, false);
assert.equal(pendingWifi.ip, '');
assert.equal(pendingWifi.rssi, 0);
const verifiedWifi = get('wifi-verified-association').wifi;
assert.equal(verifiedWifi.connected, true);
assert.equal(verifiedWifi.connecting, false);
assert.equal(verifiedWifi.saved, true);
assert.equal(verifiedWifi.ssid, 'S'.repeat(32));
for (const label of ['wifi-expired-late-association', 'wifi-sdk-failure-state']) {
  const wifi = get(label).wifi;
  assert.equal(wifi.connected, false);
  assert.equal(wifi.connecting, false);
  assert.equal(wifi.saved, false);
  assert.equal(wifi.retrying, false);
  assert(wifi.error.length > 0);
}
const retainedWifi = get('wifi-copy-old-link-retained').wifi;
assert.equal(retainedWifi.connected, true);
assert.equal(retainedWifi.saved, true);
assert.equal(retainedWifi.ssid, 'previous-network');
assert(retainedWifi.error.length > 0);
assert.equal(get('wifi-copy-allocation-failure').ok, false);
assert.equal(get('enabled-allocation-failure').ok, false);
assert.deepEqual(get('scan-dedup').networks, [
  { ssid: 'a\nb', rssi: -30 }, { ssid: 'a', rssi: -40 },
  { ssid: 'b', rssi: -50 }, { ssid: 'quote"\\', rssi: -80 },
]);
assert.equal(get('scan-bound').networks.length, 20);
assert.equal(get('scan-bound').networks.at(-1).ssid, 'network-19');
assert.equal(get('settings-success').enabled, true);
assert.equal(get('stop').enabled, false);
assert.equal(get('startup-failure').settings.enabled, false);
assert.equal(get('startup-failure').stateKey, 'SafetyFaultLatched');
assert.equal(get('sensor-read-fresh').sampleAgeMs, 25);
assert.equal(get('sensor-read-fresh').temperatureC, 25);
assert.equal(get('sensor-read-delayed').sampleAgeMs, 1500);
assert.equal(get('sensor-read-delayed').temperatureC, null);
assert.equal(get('sensor-read-delayed').sensorOk, false);
assert.equal(get('direction-started').started, true);
for (const label of ['direction-invalidation-error', 'direction-result-save-failure',
                     'direction-boot-corrupt', 'direction-apply-failure']) {
  const json = get(label);
  assert.equal(json.direction.value, 'unknown');
  assert.equal(json.direction.saved, false);
  assert(json.direction.error.length > 0);
  assert.equal(json.settings.enabled, false);
  assert.equal(json.driverAwake, false);
}
const missing = get('direction-boot-missing');
assert.equal(missing.direction.value, 'unknown');
assert.equal(missing.direction.saved, false);
assert.equal(missing.direction.error, '');
assert.equal(missing.settings.enabled, false);
assert.equal(missing.driverAwake, false);
for (const direction of ['normal', 'reversed']) {
  for (const label of [`direction-boot-${direction}`, `direction-${direction}-saved`]) {
    const json = get(label);
    assert.equal(json.direction.value, direction);
    assert.equal(json.direction.saved, true);
    assert.equal(json.direction.error, '');
    assert.equal(json.settings.enabled, false);
    assert.equal(json.driverAwake, false);
    assert.equal(json.signedDutyPercent, 0);
  }
}
assert.equal(get('direction-stop').enabled, false);
assert.equal(get('direction-unknown-enable').ok, false);
assert.equal(get('direction-unknown-disabled-settings').enabled, false);
for (const phase of ['idle', 'baseline_forward', 'forward', 'settle', 'baseline_reverse',
                     'reverse', 'complete', 'failed', 'cancelled']) {
  const json = get(`direction-phase-${phase}`);
  assert.equal(json.stateKey, 'DirectionCalibration');
  assert.equal(json.state, 10);
  assert.equal(json.direction.value, 'unknown');
  assert.equal(json.direction.saved, false);
  assert.equal(json.direction.error, 'store\\error\t');
  assert.deepEqual(json.direction.calibration, {
    phase, active: true, phaseElapsedMs: 1234, pulseMs: 3000, dutyLimitPercent: 30,
    rawDutyPercent: phase === 'forward' ? 100 : phase === 'reverse' ? -100 : 0,
    forwardDeltaC: 0.75, reverseDeltaC: -0.5, candidate: 'unknown', error: 'probe "raw"\n',
  });
  // Raw IN1/IN2 drive is not a heating/cooling inference.
  assert.equal(json.signedDutyPercent, 0);
  assert.equal(json.coolingDutyPercent, 0);
  assert.equal(json.heatingDutyPercent, 0);
}
assert.equal(get('direction-nonfinite-deltas').direction.calibration.forwardDeltaC, null);
assert.equal(get('direction-nonfinite-deltas').direction.calibration.reverseDeltaC, null);
console.log(`PASS ${parsedCount} response JSON documents parsed; snapshot, scan, and direction semantics verified`);
