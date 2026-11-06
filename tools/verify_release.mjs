import fs from "node:fs";
import path from "node:path";
import crypto from "node:crypto";
import { fileURLToPath } from "node:url";
import assert from "node:assert/strict";

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), "..");
const manifestPath = path.join(root, "docs", "发布清单.json");
const hash = (file) => crypto.createHash("sha256").update(fs.readFileSync(path.join(root, file))).digest("hex");
const ignored = new Set(["build", ".build", ".git", "node_modules", ".pio", "test-results"]);
function walk(dir) {
  return fs.readdirSync(path.join(root, dir), { withFileTypes: true }).flatMap((entry) => {
    const file = `${dir}/${entry.name}`;
    if (ignored.has(entry.name)) return [];
    return entry.isDirectory() ? walk(file) : [file];
  });
}
const files = ["README.md", "LICENSE", "CONTRIBUTING.md", "platformio.ini", "package.json",
  "package-lock.json", ".gitignore", ".gitattributes",
  ...["src", "include", "tests", "tools", "docs", "hardware", ".github"].flatMap(walk)]
  .filter((file) => file !== "docs/发布清单.json").sort();
const inputs = Object.fromEntries(files.map((file) => [file, hash(file)]));
if (process.argv.includes("--write")) {
  const native = JSON.parse(fs.readFileSync(path.join(root, "test-results/native-summary.json")));
  const browser = JSON.parse(fs.readFileSync(path.join(root, "test-results/web-qa/results.json")));
  assert(native.results.length === 5 && native.results.every((suite) => suite.status === "通过"));
  assert(browser.checks.length === 44 && browser.checks.every((check) => check.passed));
  assert.equal(browser.sha256, hash("include/web_ui.h"));
  assert.equal(browser.testSha256, hash("tests/web_ui_test.cjs"));
  const firmware = Object.fromEntries(["pl530_tec", "pl530_tec_debug"].map((env) => {
    const file = `.pio/build/${env}/firmware.bin`;
    return [env, { bytes: fs.statSync(path.join(root, file)).size, sha256: hash(file) }];
  }));
  fs.writeFileSync(manifestPath, JSON.stringify({
    date: "2026-11-07",
    description: "本次发布文件与本机测试、编译产物的校验记录，不代表实机安全验收。",
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
      firmware,
      hardwareSha256: hash("hardware/PL530.epro2"),
      limitations: "模拟测试未覆盖所有 ESP_PLATFORM 实机分支；本次未烧录、未运行 PCB DRC/ ERC、未接通 TEC。"
    }
  }, null, 2) + "\n");
  console.log("发布清单已生成。");
} else {
  const manifest = JSON.parse(fs.readFileSync(manifestPath));
  assert.deepEqual(inputs, manifest.inputs, "发布文件与清单不一致，请重新测试并生成清单。");
  console.log(`发布文件校验通过，共 ${files.length} 个文件。`);
}
