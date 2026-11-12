import fs from "node:fs";
import path from "node:path";
import crypto from "node:crypto";
import { fileURLToPath } from "node:url";
import assert from "node:assert/strict";

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), "..");
const manifestPath = path.join(root, "docs", "发布清单.json");
const hash = (file) => crypto.createHash("sha256").update(fs.readFileSync(path.join(root, file))).digest("hex");
const ignored = new Set(["build", ".build", ".git", "node_modules", ".pio", "test-results", "__pycache__"]);
function walk(dir) {
  return fs.readdirSync(path.join(root, dir), { withFileTypes: true }).flatMap((entry) => {
    const file = `${dir}/${entry.name}`;
    if (ignored.has(entry.name)) return [];
    return entry.isDirectory() ? walk(file) : [file];
  });
}
const files = ["README.md", "LICENSE", "CONTRIBUTING.md", "platformio.ini", "package.json",
  "package-lock.json", ".gitignore", ".gitattributes",
  ...["src", "include", "tests", "tools", "docs", "hardware", ".github", "firmware"].flatMap(walk)]
  .filter((file) => file !== "docs/发布清单.json").sort();
const inputs = Object.fromEntries(files.map((file) => [file, hash(file)]));
if (process.argv.includes("--write")) {
  const native = JSON.parse(fs.readFileSync(path.join(root, "test-results/native-summary.json")));
  const browser = JSON.parse(fs.readFileSync(path.join(root, "test-results/web-qa/results.json")));
  const precisionBrowser = JSON.parse(fs.readFileSync(
      path.join(root, "firmware/precision_pid/test-results/web-qa/results.json")));
  assert(native.results.length === 13 && native.results.every((suite) => suite.status === "通过"));
  assert(native.results.some((suite) => suite.suite === "controller-hardware"));
  assert(native.results.some((suite) => suite.suite === "precision-controller-hardware"));
  assert(native.results.some((suite) => suite.suite === "integration-precision-debug"));
  assert(browser.checks.length === 44 && browser.checks.every((check) => check.passed));
  assert.equal(browser.sha256, hash("include/web_ui.h"));
  assert.equal(browser.testSha256, hash("tests/web_ui_test.cjs"));
  assert(precisionBrowser.checks.length === 46 &&
      precisionBrowser.checks.every((check) => check.passed));
  assert.equal(precisionBrowser.sha256, hash("firmware/precision_pid/include/web_ui.h"));
  assert.equal(precisionBrowser.testSha256, hash("firmware/precision_pid/tests/web_ui_test.cjs"));
  const firmware = Object.fromEntries(["pl530_tec", "pl530_tec_debug", "precision_pid"].map((env) => {
    const file = env === "precision_pid"
        ? `firmware/precision_pid/.pio/build/${env}/firmware.bin`
        : `.pio/build/${env}/firmware.bin`;
    return [env, { bytes: fs.statSync(path.join(root, file)).size, sha256: hash(file) }];
  }));
  fs.writeFileSync(manifestPath, JSON.stringify({
    date: new Date().toISOString().slice(0, 10),
    description: "发布文件、主机测试与固件编译产物的校验记录。",
    inputs,
    verification: {
      native: native.results.map((suite) => ({
        ...suite,
        summary: suite.summary
          .replace("sensor tests passed", "项传感器测试通过")
          .replace("tests passed", "项测试通过")
          .replace(/PASS (\d+) response JSON documents parsed; snapshot, scan, and direction semantics verified/,
            "$1 份响应 JSON 及状态、扫描和方向语义验证通过")
      })),
      browser: { passed: browser.checks.length, total: browser.checks.length },
      precisionBrowser: { passed: precisionBrowser.checks.length, total: precisionBrowser.checks.length },
      firmware,
      hardwareSha256: hash("hardware/PL530.epro2"),
      limitations: "本次故障修复未经重新上板验证；10 mK 为精密 PID 的整定与实测目标。两版控制器均以 ESP_PLATFORM 编译并执行主机故障注入，GPIO、LEDC、ISR 与定时器仍为模拟外围，不代表真实电气时序验证。"
    }
  }, null, 2) + "\n");
  console.log("发布清单已生成。");
} else {
  const manifest = JSON.parse(fs.readFileSync(manifestPath));
  assert.deepEqual(inputs, manifest.inputs, "发布文件与清单不一致，请重新测试并生成清单。");
  console.log(`发布文件校验通过，共 ${files.length} 个文件。`);
}
