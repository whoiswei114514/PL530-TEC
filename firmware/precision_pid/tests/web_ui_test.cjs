const assert = require("node:assert/strict");
const fs = require("node:fs");
const path = require("node:path");
const crypto = require("node:crypto");
const { chromium } = require(process.env.PLAYWRIGHT_MODULE ||
  "playwright");

const project = path.resolve(__dirname, "..");
const output = path.join(project, "test-results", "web-qa");
const source = fs.readFileSync(path.join(project, "include", "web_ui.h"), "utf8");
const html = source.match(/R"rawliteral\(([\s\S]*?)\)rawliteral"/)[1];
fs.mkdirSync(output, { recursive: true });
const label = process.env.WEB_QA_LABEL || "after";

// Each regression gets a fresh page and explicit network barriers, not timing races.
async function workflowRegressions(browser, initialStatus, checks, errors) {
  async function scenario(name, run, configure = () => {}) {
    const page = await browser.newPage({ viewport: { width: 375, height: 812 } });
    page.setDefaultTimeout(6000);
    page.on("pageerror", error => errors.push(error.message));
    const state = structuredClone(initialStatus);
    state.settings.enabled = false;
    state.stateKey = "Disabled";
    state.stateText = "已停用";
    state.coolingDutyPercent = 0;
    const api = { state, mode: "normal", requests: [], bodies: [], barriers: new Map(), held: [] };
    api.hold = endpoint => {
      let release;
      const promise = new Promise(resolve => { release = resolve; });
      const gate = { promise, release, arrived: false };
      api.barriers.set(endpoint, gate);
      api.held.push(gate);
      return gate;
    };
    api.wait = async gate => {
      for (let i = 0; !gate.arrived && i < 200; i++) await page.waitForTimeout(10);
      assert.equal(gate.arrived, true, "Expected request reached its network barrier");
    };
    configure(api);
    await page.addInitScript(() => { window.setInterval = () => 0; });
    await page.route("http://127.0.0.1:18763/**", async route => {
      const endpoint = new URL(route.request().url()).pathname;
      const reply = (data, code = 200) => route.fulfill({
        status: code, contentType: "application/json", body: JSON.stringify(data)
      });
      if (endpoint === "/") return route.fulfill({ contentType: "text/html; charset=utf-8", body: html });
      assert.equal(route.request().headers()["x-pl530-request"], "1");
      const snapshot = structuredClone(state);
      const mode = api.mode;
      const gate = api.barriers.get(endpoint);
      api.requests.push(endpoint);
      if (gate) {
        api.barriers.delete(endpoint);
        gate.arrived = true;
        await gate.promise;
      }
      if (endpoint === "/api/status") {
        return mode === "error" ? reply({ ok: false, error: "Status unavailable" }, 503) : reply(snapshot);
      }
      if (endpoint === "/api/wifi/scan") {
        return reply(api.scanPolling ? { ok: true, scanning: true } :
          { ok: true, scanning: false, networks: [{ ssid: "Recovery AP", rssi: -40 }] });
      }
      if (endpoint === "/api/settings") {
        const body = Object.fromEntries(new URLSearchParams(route.request().postData()));
        api.bodies.push(body);
        if (!body.target || !body.high || !body.low) return reply({ ok: false, error: "缺少控制参数" }, 400);
        if (state.direction.calibration.active) return reply({ ok: false, error: "方向检测中" }, 409);
        state.settings = { targetC: Number(body.target), pwmHighC: Number(body.high),
          pwmLowC: Number(body.low), enabled: body.enabled === "on" };
        state.stateKey = state.settings.enabled ? "Balancing" : "Disabled";
        state.stateText = state.settings.enabled ? "热平衡调节" : "已停用";
      }
      if (endpoint === "/api/pid") {
        const body = Object.fromEntries(new URLSearchParams(route.request().postData()));
        api.pidBody = body;
        assert.equal(state.settings.enabled, false);
        Object.keys(state.pid.settings).forEach(key => state.pid.settings[key] = Number(body[key]));
      }
      if (endpoint === "/api/control/detect-direction") {
        state.settings.enabled = false;
        state.stateKey = "DirectionCalibration";
        state.stateText = "TEC 方向检测";
        state.direction.value = "unknown";
        state.direction.saved = false;
        Object.assign(state.direction.calibration, { active: true, phase: "baseline_forward" });
      }
      if (endpoint === "/api/control/stop" || endpoint === "/api/control/reset-fault") {
        state.settings.enabled = false;
        state.stateKey = "Disabled";
        state.stateText = "已停用";
        state.coolingDutyPercent = 0;
        if (state.direction.calibration.active) {
          Object.assign(state.direction.calibration, { active: false, phase: "cancelled" });
        }
      }
      return reply({ ok: true });
    });
    api.refresh = async () => {
      await page.waitForFunction(() => !refreshing);
      await page.evaluate(() => refresh());
      await page.waitForFunction(() => !refreshing);
    };
    api.settle = async () => {
      await page.evaluate(() => commandPending);
      await api.refresh();
    };
    try {
      await page.goto("http://127.0.0.1:18763/");
      await page.waitForFunction(() => document.getElementById("state").textContent !== "等待状态");
      await run(page, api);
      checks.push({ name, passed: true });
      console.log(`PASS: ${name}`);
    } catch (error) {
      checks.push({ name, passed: false, error: error.message });
      console.error(`FAIL: ${name}\n${error.stack}`);
      await page.screenshot({ path: path.join(output, `${label}-${name.split(":")[0]}.png`), fullPage: true });
    } finally {
      for (const gate of api.held) gate.release();
      await page.close();
    }
  }
  await scenario("R01: Completed calibration does not claim TEC is stopped after manual enable", async (page, api) => {
    api.state.direction.calibration.phase = "complete";
    await api.refresh();
    await page.locator("#enabled").check();
    await page.getByRole("button", { name: "保存参数", exact: true }).click();
    await api.settle();
    assert.equal(api.state.settings.enabled, true);
    assert.doesNotMatch(await page.locator("#calibrationStatus").textContent(), /TEC 保持停用|TEC 已停止/);
    await page.screenshot({ path: path.join(output, `${label}-complete-enabled.png`), fullPage: true });
  });
  await scenario("R02: Lost status locks control and reset but keeps Stop available; reconnect keeps edits", async (page, api) => {
    await page.locator("#target").fill("26");
    api.mode = "error";
    await api.refresh();
    assert.equal(await page.locator("#target").isDisabled(), true);
    assert.equal(await page.locator("#enabled").isDisabled(), true);
    assert.equal(await page.locator("#resetFault").isDisabled(), true);
    assert.equal(await page.locator("#stopOutput").isEnabled(), true);
    await page.screenshot({ path: path.join(output, `${label}-offline.png`), fullPage: true });
    api.mode = "normal";
    await api.refresh();
    assert.equal(await page.locator("#target").isEnabled(), true);
    assert.equal(await page.locator("#resetFault").isEnabled(), true);
    assert.equal(await page.locator("#target").inputValue(), "26");
  });
  await scenario("R03: Pending Stop cannot be followed by a newly queued full-power detection", async (page, api) => {
    const gate = api.hold("/api/wifi/disconnect");
    await page.locator("#disconnect").click();
    await api.wait(gate);
    await page.locator("#stopOutput").click();
    const detectLocked = await page.locator("#detectDirection").isDisabled();
    const saveLocked = await page.getByRole("button", { name: "保存参数", exact: true }).isDisabled();
    if (!detectLocked) {
      page.once("dialog", dialog => dialog.accept());
      await page.locator("#detectDirection").click();
    }
    gate.release();
    await api.settle();
    assert.equal(api.requests.includes("/api/control/detect-direction"), false);
    assert.equal(api.state.direction.calibration.active, false);
    assert.equal(detectLocked && saveLocked, true);
  });
  for (const kind of ["settings", "detect-direction"]) {
    await scenario(`${kind === "settings" ? "R04" : "R05"}: Stop cancels unsent ${kind} queued behind WiFi`, async (page, api) => {
      const gate = api.hold("/api/wifi/disconnect");
      await page.locator("#disconnect").click();
      await api.wait(gate);
      if (kind === "settings") {
        await page.locator("#enabled").check();
        await page.getByRole("button", { name: "保存参数", exact: true }).click();
      } else {
        page.once("dialog", dialog => dialog.accept());
        await page.locator("#detectDirection").click();
      }
      await page.locator("#stopOutput").click();
      gate.release();
      await api.settle();
      assert.deepEqual(api.requests.filter(p => p !== "/api/status"),
        ["/api/wifi/disconnect", "/api/control/stop"]);
      assert.equal(api.state.settings.enabled, false);
      assert.equal(api.state.direction.calibration.active, false);
    });
  }
  await scenario("R06: Detection request locks settings before its response arrives", async (page, api) => {
    const gate = api.hold("/api/control/detect-direction");
    page.once("dialog", dialog => dialog.accept());
    await page.locator("#detectDirection").click();
    await api.wait(gate);
    const locked = await page.locator("#target").isDisabled();
    gate.release();
    await api.settle();
    assert.equal(locked, true);
    assert.equal(await page.locator("#target").isDisabled(), true);
    assert.equal(await page.locator("#stopOutput").isEnabled(), true);
  });
  await scenario("R07: Dirty temperature cannot preserve enabled=true after device disables output", async (page, api) => {
    await page.locator("#target").fill("26");
    api.state.settings.enabled = false;
    await api.refresh();
    const checked = await page.locator("#enabled").isChecked();
    await page.getByRole("button", { name: "保存参数", exact: true }).click();
    await api.settle();
    assert.equal(api.state.settings.enabled, false, "Saving only a temperature edit must not re-enable TEC");
    assert.equal(checked, false);
    assert.equal(api.bodies.at(-1).target, "26");
  }, api => { api.state.settings.enabled = true; });
  await scenario("R08: Unknown direction clears checked disabled enable and never resurrects it", async (page, api) => {
    await page.locator("#enabled").check();
    api.state.direction.value = "unknown";
    api.state.direction.saved = false;
    await api.refresh();
    const wasChecked = await page.locator("#enabled").isChecked();
    assert.equal(await page.locator("#enabled").isDisabled(), true);
    await page.getByRole("button", { name: "保存参数", exact: true }).click();
    await api.settle();
    assert.equal(api.bodies.at(-1).enabled, undefined, "Disabled checkbox must stay omitted from FormData");
    assert.equal(wasChecked, false, "Disabled checkbox must not misleadingly remain checked");
    api.state.direction.value = "normal";
    api.state.direction.saved = true;
    await api.refresh();
    assert.equal(await page.locator("#enabled").isChecked(), false);
  });
  await scenario("R09: Pre-command status response cannot unlock active calibration", async (page, api) => {
    const gate = api.hold("/api/status");
    await page.evaluate(() => { void refresh(); });
    await api.wait(gate);
    page.once("dialog", dialog => dialog.accept());
    await page.locator("#detectDirection").click();
    await page.evaluate(() => commandPending);
    gate.release();
    await page.waitForFunction(() => !refreshing);
    assert.equal(await page.locator("#target").isDisabled(), true);
    assert.match(await page.locator("#calibrationStatus").textContent(), /采集正向/);
  });
  await scenario("R10: No control unlock before the first successful status", async (page) => {
    assert.equal(await page.locator("#enabled").isDisabled(), true);
    assert.equal(await page.getByRole("button", { name: "保存参数", exact: true }).isDisabled(), true);
    assert.equal(await page.locator("#resetFault").isDisabled(), true);
    assert.equal(await page.locator("#stopOutput").isEnabled(), true);
  }, api => { api.mode = "error"; });
  await scenario("R11: Explicit unsaved enable survives an unchanged disabled status", async (page, api) => {
    await page.locator("#enabled").check();
    await api.refresh();
    assert.equal(await page.locator("#enabled").isChecked(), true);
    await page.getByRole("button", { name: "保存参数", exact: true }).click();
    await api.settle();
    assert.equal(api.bodies.at(-1).enabled, "on");
  });
  await scenario("R12: Lost connection during calibration never unlocks settings on reconnect", async (page, api) => {
    page.once("dialog", dialog => dialog.accept());
    await page.locator("#detectDirection").click();
    await api.settle();
    api.mode = "error";
    await api.refresh();
    assert.equal(await page.locator("#target").isDisabled(), true);
    assert.equal(await page.locator("#stopOutput").isEnabled(), true);
    api.mode = "normal";
    await api.refresh();
    assert.equal(await page.locator("#target").isDisabled(), true);
    assert.equal(await page.locator("#resetFault").isDisabled(), true);
    await page.locator("#stopOutput").click();
    await api.settle();
    assert.equal(await page.locator("#target").isEnabled(), true);
    assert.equal(await page.locator("#enabled").isDisabled(), true);
    assert.match(await page.locator("#calibrationStatus").textContent(), /已取消/);
    await page.screenshot({ path: path.join(output, `${label}-stopped-calibration.png`), fullPage: true });
  });
  await scenario("R13: Stop acknowledgement alone cannot unlock until fresh status succeeds", async (page, api) => {
    const stop = api.hold("/api/control/stop");
    await page.locator("#stopOutput").click();
    await api.wait(stop);
    const status = api.hold("/api/status");
    stop.release();
    await api.wait(status);
    const locked = await page.getByRole("button", { name: "保存参数", exact: true }).isDisabled();
    const detectLocked = await page.locator("#detectDirection").isDisabled();
    status.release();
    await api.settle();
    assert.equal(locked && detectLocked, true);
    assert.equal(await page.locator("#target").isEnabled(), true);
    assert.equal(await page.locator("#enabled").isChecked(), false);
  });
  await scenario("R14: Stop follows already-sent detection and leaves calibration cancelled", async (page, api) => {
    const gate = api.hold("/api/control/detect-direction");
    page.once("dialog", dialog => dialog.accept());
    await page.locator("#detectDirection").click();
    await api.wait(gate);
    await page.locator("#stopOutput").click();
    assert.equal(api.requests.includes("/api/control/stop"), false);
    gate.release();
    await api.settle();
    assert.deepEqual(api.requests.filter(p => p !== "/api/status"),
      ["/api/control/detect-direction", "/api/control/stop"]);
    assert.equal(api.state.direction.calibration.active, false);
    assert.equal(api.state.settings.enabled, false);
    assert.equal(await page.locator("#enabled").isChecked(), false);
  });
  await scenario("R15: Stop cancels queued reset without breaking subsequent manual control", async (page, api) => {
    const gate = api.hold("/api/wifi/disconnect");
    await page.locator("#disconnect").click();
    await api.wait(gate);
    page.once("dialog", dialog => dialog.accept());
    await page.locator("#resetFault").click();
    const resetLocked = await page.locator("#resetFault").isDisabled();
    await page.locator("#stopOutput").click();
    gate.release();
    await api.settle();
    assert.equal(resetLocked, true);
    assert.equal(api.requests.includes("/api/control/reset-fault"), false);
    await page.locator("#enabled").check();
    await page.getByRole("button", { name: "保存参数", exact: true }).click();
    await api.settle();
    assert.equal(api.state.settings.enabled, true);
  });
  await scenario("R16: Late failed pre-command status is discarded and refreshed after detection", async (page, api) => {
    api.mode = "error";
    const gate = api.hold("/api/status");
    await page.evaluate(() => { void refresh(); });
    await api.wait(gate);
    api.mode = "normal";
    page.once("dialog", dialog => dialog.accept());
    await page.locator("#detectDirection").click();
    await page.evaluate(() => commandPending);
    gate.release();
    await page.waitForFunction(() => !refreshing);
    assert.equal(await page.locator("#state").textContent(), "TEC 方向检测");
    assert.equal(await page.locator("#target").isDisabled(), true);
    assert.match(await page.locator("#calibrationStatus").textContent(), /采集正向/);
  });
  await scenario("R17: Failed post-save status keeps controls locked until recovery", async (page, api) => {
    await page.locator("#target").fill("26");
    await page.locator("#enabled").check();
    api.mode = "error";
    await page.getByRole("button", { name: "保存参数", exact: true }).click();
    await api.settle();
    assert.equal(api.state.settings.enabled, true);
    assert.deepEqual(api.bodies.at(-1), { target: "26", high: "30", low: "20", enabled: "on" });
    assert.equal(await page.locator("#target").isDisabled(), true);
    assert.equal(await page.locator("#enabled").isChecked(), false);
    assert.equal(await page.locator("#stopOutput").isEnabled(), true);
    assert.match(await page.locator("#faultNotice").textContent(), /实际状态未知/);
    api.mode = "normal";
    await api.refresh();
    assert.equal(await page.locator("#target").isEnabled(), true);
    assert.equal(await page.locator("#enabled").isChecked(), true);
  });

  async function fillWifi(page) {
    await page.locator("#ssidSelect").evaluate(select => {
      select.replaceChildren(new Option("Recovery AP", "Recovery AP"));
    });
    await page.locator("#wifiPass").fill("recovery-password");
  }
  function enabledSta(api) {
    api.state.settings.enabled = true;
    api.state.stateKey = "NormalControl";
    api.state.stateText = "温度调节";
    Object.assign(api.state.wifi, { connected: true, ssid: "Current STA", ip: "192.168.50.25" });
  }
  async function clickWifi(page, operation) {
    if (operation === "connect") {
      await page.getByRole("button", { name: "连接", exact: true }).click();
    } else {
      await page.locator("#disconnect").click();
    }
  }

  for (const operation of ["connect", "disconnect"]) {
    await scenario(`${operation === "connect" ? "R18" : "R19"}: Stop cancels unsent WiFi ${operation} behind settings`, async (page, api) => {
      await fillWifi(page);
      const settings = api.hold("/api/settings");
      await page.getByRole("button", { name: "保存参数", exact: true }).click();
      await api.wait(settings);
      await clickWifi(page, operation);
      assert.deepEqual(api.requests.filter(p => p !== "/api/status"), ["/api/settings"]);
      assert.equal(await page.locator("#stopOutput").isEnabled(), true);
      await page.locator("#stopOutput").click();
      settings.release();
      await api.settle();
      assert.deepEqual(api.requests.filter(p => p !== "/api/status"),
        ["/api/settings", "/api/control/stop"]);
      assert.equal(api.state.settings.enabled, false);
      if (operation === "connect") {
        assert.equal(await page.locator("#wifiPass").inputValue(), "recovery-password",
          "An unsent cancelled connection must not erase credentials as if it succeeded");
      }
    }, enabledSta);
  }
  for (const sent of ["connect", "disconnect"]) {
    await scenario(`${sent === "connect" ? "R20" : "R21"}: Stop follows sent WiFi ${sent} but cancels the queued opposite action`, async (page, api) => {
      await fillWifi(page);
      const endpoint = `/api/wifi/${sent}`;
      const gate = api.hold(endpoint);
      await clickWifi(page, sent);
      await api.wait(gate);
      await clickWifi(page, sent === "connect" ? "disconnect" : "connect");
      await page.locator("#stopOutput").click();
      assert.deepEqual(api.requests.filter(p => p !== "/api/status"), [endpoint],
        "Stop must not overtake an already-sent request");
      gate.release();
      await api.settle();
      assert.deepEqual(api.requests.filter(p => p !== "/api/status"),
        [endpoint, "/api/control/stop"]);
      assert.equal(api.state.settings.enabled, false);
    }, enabledSta);
  }
  await scenario("R22: WiFi handlers and queue reject new mutations throughout queued and active Stop", async (page, api) => {
    await fillWifi(page);
    const settings = api.hold("/api/settings");
    const stop = api.hold("/api/control/stop");
    await page.getByRole("button", { name: "保存参数", exact: true }).click();
    await api.wait(settings);
    await page.locator("#stopOutput").click();
    async function attemptWhileStopping() {
      assert.equal(await page.getByRole("button", { name: "连接", exact: true }).isDisabled(), true);
      assert.equal(await page.locator("#disconnect").isDisabled(), true);
      assert.equal(await page.locator("#scan").isDisabled(), true);
      // Bypass disabled-button browser behavior to exercise the handler guards.
      await page.evaluate(() => {
        document.getElementById("wifiForm").dispatchEvent(new Event("submit", { bubbles: true, cancelable: true }));
        document.getElementById("disconnect").dispatchEvent(new MouseEvent("click", { bubbles: true }));
        document.getElementById("scan").dispatchEvent(new MouseEvent("click", { bubbles: true }));
        void command(() => requestJson("/api/wifi/connect", { method: "POST" }))
          .then(() => { window.__wifiAcceptedDuringStop = true; }, () => {});
      });
    }
    await attemptWhileStopping();
    assert.equal(api.requests.includes("/api/control/stop"), false);
    settings.release();
    await api.wait(stop);
    await attemptWhileStopping();
    stop.release();
    await api.settle();
    assert.deepEqual(api.requests.filter(p => p !== "/api/status"),
      ["/api/settings", "/api/control/stop"]);
    assert.notEqual(await page.evaluate(() => window.__wifiAcceptedDuringStop), true);
    assert.equal(api.state.settings.enabled, false);
    assert.equal(await page.getByRole("button", { name: "连接", exact: true }).isEnabled(), true);
    assert.equal(await page.locator("#disconnect").isEnabled(), true);
    assert.equal(await page.locator("#scan").isEnabled(), true);
  }, enabledSta);
  await scenario("R23: Lost status cancels stale queued WiFi but permits fresh intentional recovery", async (page, api) => {
    await fillWifi(page);
    // Hold an unsent queue entry, without an active request suppressing status refresh.
    await page.evaluate(() => {
      commandPending = commandPending.then(() => new Promise(resolve => {
        window.__releaseUnsentNetwork = resolve;
      }));
    });
    await page.waitForFunction(() => typeof window.__releaseUnsentNetwork === "function");
    await clickWifi(page, "connect");
    api.mode = "error";
    await api.refresh();
    assert.equal(await page.locator("#target").isDisabled(), true);
    await page.evaluate(() => window.__releaseUnsentNetwork());
    await api.settle();
    assert.deepEqual(api.requests.filter(p => p !== "/api/status"), []);
    assert.equal(await page.getByRole("button", { name: "连接", exact: true }).isEnabled(), true);
    await clickWifi(page, "connect");
    await api.settle();
    assert.deepEqual(api.requests.filter(p => p !== "/api/status"), ["/api/wifi/connect"]);
    assert.equal(await page.locator("#target").isDisabled(), true);
    assert.equal(await page.locator("#stopOutput").isEnabled(), true);
    api.mode = "normal";
    await api.refresh();
    assert.equal(await page.locator("#target").isEnabled(), true);
  });
  await scenario("R24: Fresh WiFi reconnect remains usable after Stop even before status recovers", async (page, api) => {
    await fillWifi(page);
    api.mode = "error";
    await page.locator("#stopOutput").click();
    await api.settle();
    assert.equal(await page.locator("#target").isDisabled(), true);
    assert.equal(await page.locator("#enabled").isChecked(), false);
    assert.equal(await page.getByRole("button", { name: "连接", exact: true }).isEnabled(), true);
    await clickWifi(page, "connect");
    await api.settle();
    assert.deepEqual(api.requests.filter(p => p !== "/api/status"),
      ["/api/control/stop", "/api/wifi/connect"]);
    assert.equal(api.state.settings.enabled, false);
    assert.equal(await page.locator("#stopOutput").isEnabled(), true);
    api.mode = "normal";
    await api.refresh();
    assert.equal(await page.locator("#target").isEnabled(), true);
    assert.equal(await page.locator("#enabled").isChecked(), false);
  }, enabledSta);
  await scenario("R25: Stop also cancels an unsent scan that could start a radio operation", async (page, api) => {
    const settings = api.hold("/api/settings");
    await page.getByRole("button", { name: "保存参数", exact: true }).click();
    await api.wait(settings);
    await page.locator("#scan").click();
    await page.locator("#stopOutput").click();
    settings.release();
    await api.settle();
    assert.deepEqual(api.requests.filter(p => p !== "/api/status"),
      ["/api/settings", "/api/control/stop"]);
    assert.equal(await page.locator("#scan").isEnabled(), true);
    assert.equal(api.state.settings.enabled, false);
  }, enabledSta);
  await scenario("R26: A pre-Stop scan cannot resume polling after Stop; a fresh scan can", async (page, api) => {
    // Hold the existing 300 ms scan-poll delay so Stop finishes before continuation.
    await page.evaluate(() => {
      const schedule = window.setTimeout;
      window.setTimeout = (callback, ms, ...args) => {
        if (ms === 300) {
          window.__resumeScanPoll = () => callback(...args);
          return 0;
        }
        return schedule(callback, ms, ...args);
      };
    });
    await page.locator("#scan").click();
    await page.waitForFunction(() => typeof window.__resumeScanPoll === "function");
    await page.locator("#stopOutput").click();
    await api.settle();
    assert.equal(await page.locator("#scan").isDisabled(), true,
      "Stop cleanup must not unlock a scan handler that is still awaiting its poll delay");
    api.scanPolling = false;
    await page.evaluate(() => window.__resumeScanPoll());
    await page.waitForFunction(() => !wifiScanning);
    await api.settle();
    assert.deepEqual(api.requests.filter(p => p !== "/api/status"),
      ["/api/wifi/scan", "/api/control/stop"]);
    await page.locator("#scan").click();
    await page.waitForFunction(() => !wifiScanning);
    await api.settle();
    assert.deepEqual(api.requests.filter(p => p !== "/api/status"),
      ["/api/wifi/scan", "/api/control/stop", "/api/wifi/scan"]);
    assert.equal(await page.locator("#ssidSelect").inputValue(), "Recovery AP");
  }, api => { api.scanPolling = true; });
  await scenario("R27: PID settings save only while stopped", async (page, api) => {
    await page.locator("#kp").fill("32.125");
    await page.locator("#savePid").click();
    await api.settle();
    assert.equal(api.pidBody.kp, "32.125");
    assert.equal(api.state.pid.settings.kp, 32.125);
    assert.equal(api.state.settings.enabled, false);
    assert.equal(await page.locator("#kp").inputValue(), "32.125");
    api.state.settings.enabled = true;
    await api.refresh();
    assert.equal(await page.locator("#savePid").isDisabled(), true);
    assert.equal(await page.locator("#kp").isDisabled(), true);
  });
  await scenario("R28: Zero variation above target is displayed as offset correction", async (page) => {
    assert.equal(await page.locator("#maxError").textContent(), "31.25 mK");
    assert.equal(await page.locator("#peakToPeak").textContent(), "0.00 mK");
    assert.equal(await page.locator("#pidDecision").textContent(), "持续温差修正");
  }, api => {
    api.state.pid.decision = "correcting_offset";
    api.state.pid.stability.maximumErrorMk = 31.25;
    api.state.pid.stability.peakToPeakMk = 0;
  });
}

async function main() {
  const browser = await chromium.launch({
    ...(process.env.CHROME_PATH ? { executablePath: process.env.CHROME_PATH } : {}),
    headless: true
  });
  const checks = [];
  const errors = [];
  let status = {
    sensorOk: true, temperatureC: 25.12, coolingDutyPercent: 10, heatingDutyPercent: 0,
    slopeCPerSecond: -0.02, balanceDrivePercent: -10,
    stateKey: "Balancing", stateText: "热平衡调节", sensorError: "",
    settings: { targetC: 25, pwmHighC: 30, pwmLowC: 20, enabled: true },
    wifi: { connected: false, connecting: false, ssid: "", ip: "", rssi: 0 },
    direction: { value: "normal", saved: true, error: "", calibration: {
      phase: "idle", active: false, rawDutyPercent: 0, phaseElapsedMs: 0, pulseMs: 3000,
      forwardDeltaC: null, reverseDeltaC: null, candidate: "unknown", error: ""
    } },
    pid: { decision: "regulating", settings: {kp:20,ki:0.2,kd:40,
      measurementTau:0.6,derivativeTau:2,outputLimit:30,slewRate:15,integralLimit:30},
      stability: {peakToPeakMk:15.625,maximumErrorMk:7.8125,durationMs:120000,
        ready:true,within10Mk:true}}
  };
  let statusMode = "normal";
  const initialStatus = structuredClone(status);
  let resetReject = true;
  let disconnectReject = true;
  let scanReject = false;
  let scanRequests = 0;
  let settingsBody = null;
  let wifiBody = null;
  let resetRequests = 0;
  let stopRequests = 0;
  let stopReject = false;
  let settingsDelayMs = 0;
  const commandOrder = [];
  let detectionRequests = 0;
  let detectionReject = false;
  const ok = name => { checks.push({ name, passed: true }); console.log(`PASS: ${name}`); };
  try {
    const page = await browser.newPage({ viewport: { width: 1440, height: 900 } });
    page.on("pageerror", error => errors.push(error.message));
    await page.addInitScript(() => {
      const original = window.setInterval;
      window.__testIntervals = [];
      window.setInterval = (...args) => {
        const id = original(...args);
        window.__testIntervals.push(id);
        return id;
      };
    });
    await page.route("http://127.0.0.1:18763/**", async route => {
      const url = new URL(route.request().url());
      const reply = (data, code = 200) => route.fulfill({
        status: code, contentType: "application/json", body: JSON.stringify(data)
      });
      if (url.pathname === "/") {
        return route.fulfill({ contentType: "text/html; charset=utf-8", body: html });
      }
      assert.equal(route.request().headers()["x-pl530-request"], "1",
        "All API calls must carry the same-origin request marker");
      if (url.pathname === "/api/status") {
        if (statusMode === "error") return reply({ ok: false, error: "Status unavailable" }, 503);
        if (statusMode === "hang") return;
        return reply(status);
      }
      if (url.pathname === "/api/settings") {
        commandOrder.push("settings");
        settingsBody = Object.fromEntries(new URLSearchParams(route.request().postData()));
        if (settingsDelayMs) await new Promise(resolve => setTimeout(resolve, settingsDelayMs));
        const target = Number(settingsBody.target);
        const low = Number(settingsBody.low);
        const high = Number(settingsBody.high);
        if (!(low < target && target < high)) {
          return reply({ ok: false, error: "温度参数无效" }, 400);
        }
        status.settings = { targetC: target, pwmHighC: high, pwmLowC: low,
          enabled: settingsBody.enabled === "on" };
        return reply({ ok: true });
      }
      if (url.pathname === "/api/control/reset-fault") {
        resetRequests++;
        if (!resetReject) status.settings.enabled = false;
        return resetReject ? reply({ ok: false, error: "输出保护未就绪，请检查后重启" }, 409)
          : reply({ ok: true });
      }
      if (url.pathname === "/api/control/stop") {
        commandOrder.push("stop");
        stopRequests++;
        if (stopReject) return reply({ ok: false, error: "设备无响应" }, 503);
        status.settings.enabled = false;
        if (status.direction.calibration.active) {
          status.direction.calibration.active = false;
          status.direction.calibration.phase = "cancelled";
          status.direction.calibration.rawDutyPercent = 0;
          status.stateKey = "Disabled";
          status.stateText = "已停用";
        }
        return reply({ ok: true, enabled: false });
      }
      if (url.pathname === "/api/control/detect-direction") {
        detectionRequests++;
        assert.equal(new URLSearchParams(route.request().postData()).get("confirm"), "full_power");
        if (detectionReject) return reply({ ok: false, error: "检测条件不满足" }, 409);
        status.settings.enabled = false;
        status.stateKey = "DirectionCalibration";
        status.stateText = "TEC 方向检测";
        status.direction = { value: "unknown", saved: false, error: "", calibration: {
          phase: "baseline_forward", active: true, rawDutyPercent: 0, phaseElapsedMs: 0,
          pulseMs: 3000, forwardDeltaC: null, reverseDeltaC: null, candidate: "unknown", error: ""
        } };
        return reply({ ok: true, started: true });
      }
      if (url.pathname === "/api/wifi/disconnect") {
        return disconnectReject ? reply({ ok: false, error: "断开失败" }, 500) : reply({ ok: true });
      }
      if (url.pathname === "/api/wifi/scan") {
        if (scanReject) return reply({ ok: false, error: "扫描过于频繁" }, 429);
        scanRequests++;
        return reply(scanRequests === 1 ? { ok: true, scanning: true } :
          { ok: true, scanning: false, networks: [
            { ssid: 'Lab <img src=x onerror=alert(1)>', rssi: -42 },
            { ssid: "PL530 Test", rssi: -64 }
          ] });
      }
      if (url.pathname === "/api/wifi/connect") {
        wifiBody = Object.fromEntries(new URLSearchParams(route.request().postData()));
        return reply({ ok: true });
      }
      return reply({ ok: false }, 404);
    });
    const refresh = async () => {
      await page.waitForFunction(() => !refreshing);
      await page.evaluate(() => refresh());
    };
    await page.goto("http://127.0.0.1:18763/");
    await page.waitForFunction(() => document.getElementById("temp").textContent === "25.1200");
    await page.waitForTimeout(1100);
    await page.evaluate(() => window.__testIntervals.forEach(clearInterval));
    ok("Embedded page loads and refreshes real status fields");

    for (const width of [320, 375, 390, 480, 768, 820, 821, 1024, 1440]) {
      await page.setViewportSize({ width, height: width < 600 ? 812 : 900 });
      const issues = await page.evaluate(() => {
        const bad = [];
        if (document.documentElement.scrollWidth > innerWidth) bad.push("page horizontal overflow");
        for (const el of document.querySelectorAll("input,select,button,.temp-value,.state")) {
          const rect = el.getBoundingClientRect();
          if (rect.left < 0 || rect.right > innerWidth + 1) bad.push(el.id || el.className);
        }
        const a = document.querySelector(".temp-value").getBoundingClientRect();
        const b = document.querySelector(".state").getBoundingClientRect();
        if (a.left < b.right && a.right > b.left && a.top < b.bottom && a.bottom > b.top) {
          bad.push("temperature/state overlap");
        }
        return bad;
      });
      assert.deepEqual(issues, [], `Viewport ${width}`);
      if (width === 1440) await page.screenshot({ path: path.join(output, "desktop.png"), fullPage: true });
    }
    ok("Nine widths 320-1440px: no horizontal overflow or temperature/state overlap");
    await page.locator("#target").fill("26");
    await refresh();
    assert.equal(await page.locator("#target").inputValue(), "26");
    await page.locator("#enabled").uncheck();
    await page.getByRole("button", { name: "保存参数", exact: true }).click();
    await page.waitForFunction(() => document.getElementById("message").textContent === "控制参数已保存");
    assert.equal(settingsBody.target, "26");
    assert.equal(settingsBody.enabled, undefined);
    ok("Dirty values survive refresh; submitted settings preserve unchecked checkbox semantics");
    await page.locator("#low").fill("27");
    await page.getByRole("button", { name: "保存参数", exact: true }).click();
    await page.waitForFunction(() => document.getElementById("message").textContent === "温度参数无效");
    assert.match(await page.locator("#message").getAttribute("class"), /bad/);
    ok("Settings rejection is visible and not reported as success");
    await page.locator("#low").fill("20");

    const faults = [
      ["SensorTimeoutLatched", "温度样本超时锁定", "温度样本超时"],
      ["SafetyFaultLatched", "输出保护初始化失败", "初始化失败"],
      ["DriverFaultLatched", "驱动器故障锁定", "驱动器故障"],
      ["ThermalFaultLatched", "热失控故障锁定", "热失控保护"],
      ["HardOverTemperature", "超温紧急制冷", "全力制冷"],
      ["HardUnderTemperature", "低温紧急加热", "全力加热"]
    ];
    for (const [key, text, message] of faults) {
      status.stateKey = key;
      status.stateText = text;
      status.sensorOk = !key.endsWith("Latched");
      status.coolingDutyPercent = key === "HardOverTemperature" ? 100 : 0;
      status.heatingDutyPercent = key === "HardUnderTemperature" ? 100 : 0;
      status.slopeCPerSecond = 0;
      status.balanceDrivePercent = 0;
      await refresh();
      assert.match(await page.locator("#state").getAttribute("class"), /bad/);
      assert.match(await page.locator("#message").textContent(), new RegExp(message));
      if (!status.sensorOk) assert.equal(await page.locator("#temp").textContent(), "--");
    }
    status.sensorOk = false;
    status.stateKey = "SensorTimeoutLatched";
    status.stateText = "温度样本超时锁定";
    status.heatingDutyPercent = 0;
    await page.setViewportSize({ width: 375, height: 812 });
    await refresh();
    await page.screenshot({ path: path.join(output, "mobile-fault.png"), fullPage: true });
    ok("All six fault/emergency states display correct severity and retain specific fault messages");
    await page.waitForTimeout(4200);
    assert.equal(await page.locator("#faultNotice").isVisible(), true);
    assert.match(await page.locator("#faultNotice").textContent(), /温度样本超时/);
    ok("Fault remains visible after transient message expires");
    page.once("dialog", dialog => dialog.dismiss());
    await page.locator("#resetFault").click();
    assert.equal(resetRequests, 0);
    page.once("dialog", dialog => dialog.accept());
    await page.locator("#resetFault").click();
    await page.waitForFunction(() => document.getElementById("message").textContent.includes("请检查后重启"));
    assert.match(await page.locator("#message").getAttribute("class"), /bad/);
    resetReject = false;
    page.once("dialog", dialog => dialog.accept());
    await page.locator("#resetFault").click();
    await page.waitForFunction(() => document.getElementById("message").textContent === "已解除，TEC 保持停用");
    ok("Reset cancellation, HTTP409 rejection and successful disabled reset are distinct");
    assert.equal(await page.locator("#enabled").isChecked(), false);
    await page.locator("#target").fill("");
    await page.locator("#stopOutput").click();
    await page.waitForFunction(() => document.getElementById("message").textContent === "TEC 已停止");
    assert.equal(stopRequests, 1);
    assert.equal(await page.locator("#enabled").isChecked(), false);
    assert.equal(await page.locator("#target").inputValue(), "");
    stopReject = true;
    await page.locator("#stopOutput").click();
    await page.waitForFunction(() => document.getElementById("message").textContent.includes("停止未确认"));
    assert.match(await page.locator("#message").getAttribute("class"), /bad/);
    await page.locator("#target").fill("26");
    ok("Stop bypasses invalid settings without erasing edits; failures never claim stop succeeded");
    stopReject = false;
    settingsDelayMs = 700;
    await page.locator("#enabled").check();
    const previousCommands = commandOrder.length;
    await page.getByRole("button", { name: "保存参数", exact: true }).click();
    await page.waitForFunction(() => settingsBusy);
    await page.locator("#stopOutput").click();
    await page.waitForFunction(() => document.getElementById("message").textContent === "TEC 已停止");
    assert.deepEqual(commandOrder.slice(previousCommands), ["settings", "stop"]);
    assert.equal(status.settings.enabled, false);
    assert.equal(await page.locator("#enabled").isChecked(), false);
    settingsDelayMs = 0;
    ok("Stop requested during parameter save runs afterwards and leaves output disabled");

    await page.locator("#disconnect").click();
    await page.waitForFunction(() => document.getElementById("message").textContent === "断开失败");
    disconnectReject = false;
    await page.locator("#disconnect").click();
    await page.waitForFunction(() => document.getElementById("message").textContent === "STA 连接已断开并清除");
    ok("Disconnect failure is not reported as success");
    await page.locator("#scan").click();
    await page.waitForFunction(() => document.getElementById("ssidSelect").options.length === 2);
    assert.equal(await page.locator("#ssidSelect img").count(), 0);
    assert.equal(await page.locator("#scan").isEnabled(), true);
    await page.locator("#ssidSelect").selectOption("PL530 Test");
    await page.locator("#wifiPass").fill("test-password-123");
    await page.getByRole("button", { name: "连接", exact: true }).click();
    await page.waitForFunction(() => document.getElementById("message").textContent === "已开始连接，请稍候");
    assert.deepEqual(wifiBody, { ssid: "PL530 Test", password: "test-password-123" });
    assert.equal(await page.locator("#wifiPass").inputValue(), "");
    scanReject = true;
    await page.locator("#scan").click();
    await page.waitForFunction(() => document.getElementById("message").textContent === "扫描过于频繁");
    assert.equal(await page.locator("#scan").isEnabled(), true);
    ok("Asynchronous SSID scan, untrusted SSID text, connection form and scan errors work");
    status.configurationError = "参数保存失败，TEC 已停用";
    status.wifi.error = "WiFi 连接超时，请检查 SSID 和密码";
    await refresh();
    assert.equal(await page.locator("#configurationError").isVisible(), true);
    assert.match(await page.locator("#wifiStatus").textContent(), /WiFi 连接超时/);
    ok("Configuration and WiFi failures are visible in persistent status");
    status.sensorOk = true;
    status.temperatureC = 25;
    status.stateKey = "Disabled";
    status.stateText = "已停用";
    status.settings.enabled = false;
    status.configurationError = "";
    status.wifi.error = "";
    status.direction.value = "unknown";
    status.direction.saved = false;
    await refresh();
    assert.equal(await page.locator("#enabled").isEnabled(), false);
    assert.equal(await page.locator("#detectDirection").isEnabled(), true);
    page.once("dialog", dialog => dialog.dismiss());
    await page.locator("#detectDirection").click();
    assert.equal(detectionRequests, 0);
    detectionReject = true;
    page.once("dialog", dialog => dialog.accept());
    await page.locator("#detectDirection").click();
    await page.waitForFunction(() => document.getElementById("message").textContent === "检测条件不满足");
    detectionReject = false;
    page.once("dialog", dialog => {
      assert.match(dialog.message(), /3 秒/);
      assert.match(dialog.message(), /散热器/);
      dialog.accept();
    });
    await page.locator("#detectDirection").click();
    await page.waitForFunction(() => document.getElementById("calibrationStatus").textContent.includes("采集正向"));
    assert.equal(detectionRequests, 2);
    assert.equal(await page.locator("#target").isEnabled(), false);
    assert.equal(await page.locator("#detectDirection").isEnabled(), false);
    assert.equal(await page.locator("#stopOutput").isEnabled(), true);
    ok("Direction detection requires explicit confirmation; unknown direction cannot enable normal control");

    const calibration = status.direction.calibration;
    calibration.phase = "forward";
    calibration.rawDutyPercent = 100;
    calibration.phaseElapsedMs = 1500;
    await refresh();
    assert.match(await page.locator("#calibrationStatus").textContent(), /正向 IN1 限幅脉冲/);
    assert.equal(await page.locator("#calibrationProgress").isVisible(), true);
    assert.equal(await page.locator("#calibrationProgress").getAttribute("value"), "1500");
    await page.screenshot({ path: path.join(output, "mobile-calibration.png"), fullPage: true });
    calibration.phase = "reverse";
    calibration.rawDutyPercent = -100;
    calibration.forwardDeltaC = -0.45;
    calibration.phaseElapsedMs = 3001;
    await refresh();
    assert.match(await page.locator("#calibrationStatus").textContent(), /反向 IN2 限幅脉冲/);
    assert.equal(await page.locator("#calibrationProgress").getAttribute("value"), "3000");
    await page.locator("#stopOutput").click();
    await page.waitForFunction(() => document.getElementById("message").textContent === "TEC 已停止");
    await refresh();
    assert.match(await page.locator("#calibrationStatus").textContent(), /已取消/);
    assert.equal(await page.locator("#enabled").isEnabled(), false);
    ok("Both raw test directions, pulse progress and cancellation are shown without inventing a heat/cool mapping");

    calibration.phase = "failed";
    calibration.error = "response_ambiguous";
    await refresh();
    assert.match(await page.locator("#calibrationStatus").textContent(), /温差不足/);
    assert.equal(await page.locator("#enabled").isEnabled(), false);
    calibration.phase = "complete";
    calibration.error = "";
    calibration.candidate = "reversed";
    calibration.reverseDeltaC = -0.4;
    status.direction.error = "方向检测完成，但保存失败";
    await refresh();
    assert.match(await page.locator("#calibrationStatus").textContent(), /保存失败/);
    assert.equal(await page.locator("#enabled").isEnabled(), false);
    status.direction.error = "";
    status.direction.value = "reversed";
    status.direction.saved = true;
    await refresh();
    assert.match(await page.locator("#directionStatus").textContent(), /IN1 制热 \/ IN2 制冷/);
    assert.equal(await page.locator("#enabled").isEnabled(), true);
    assert.equal(await page.locator("#enabled").isChecked(), false);
    ok("Ambiguous or unsaved detection stays unconfirmed; saved reversed mapping stays disabled until manual enable");

    statusMode = "error";
    await refresh();
    for (const id of ["temp", "cooling", "heating", "slope", "balance"]) {
      assert.equal(await page.locator(`#${id}`).textContent(), "--");
    }
    assert.equal(await page.locator("#state").textContent(), "连接中断");
    statusMode = "normal";
    await refresh();
    assert.equal(await page.locator("#state").textContent(), status.stateText);
    ok("HTTP status failure clears stale live metrics; reconnect restores fault state");
    statusMode = "hang";
    const started = Date.now();
    await refresh();
    assert.ok(Date.now() - started >= 3900 && Date.now() - started < 6500);
    assert.equal(await page.locator("#state").textContent(), "连接中断");
    ok("Unresponsive status fetch times out without claiming TEC is off");
    await page.close();
    await workflowRegressions(browser, initialStatus, checks, errors);
    assert.deepEqual(errors, []);
    ok("No uncaught browser JavaScript errors");
    const resultFile = path.join(output, `results-${label}.json`);
    fs.writeFileSync(resultFile, JSON.stringify({
      time: new Date().toISOString(), source: "include/web_ui.h",
      sha256: crypto.createHash("sha256").update(source).digest("hex"),
      testSha256: crypto.createHash("sha256").update(fs.readFileSync(__filename)).digest("hex"),
      browser: browser.version(), checks, errors,
      limitations: "Mock HTTP API checks embedded HTML/JS only; no ESP32 radio, GPIO or real thermal plant exercised."
    }, null, 2) + "\n");
    if (label === "after") fs.copyFileSync(resultFile, path.join(output, "results.json"));
    console.log(`SUMMARY: ${checks.filter(check => check.passed).length}/${checks.length} passed; source ${crypto.createHash("sha256").update(source).digest("hex")}`);
    assert.equal(checks.every(check => check.passed), true, "Workflow regressions failed; see results and screenshots");
  } finally {
    await browser.close();
  }
}

main().catch(error => { console.error(error); process.exitCode = 1; });
