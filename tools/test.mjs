import fs from "node:fs";
import path from "node:path";
import { fileURLToPath } from "node:url";
import { spawnSync } from "node:child_process";

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), "..");
const build = path.join(root, "test-results", "native");
fs.mkdirSync(build, { recursive: true });
const compiler = process.env.CXX || "g++";
const env = { ...process.env, PATH: `${path.dirname(compiler)}${path.delimiter}${process.env.PATH}` };
const flags = ["-std=c++17", "-Wall", "-Wextra", "-Werror", "-pedantic"];
const results = [];
function run(command, args, label) {
  const result = spawnSync(command, args, { cwd: root, env, encoding: "utf8", maxBuffer: 16 * 1024 * 1024 });
  fs.writeFileSync(path.join(build, `${label}.log`), (result.stdout || "") + (result.stderr || ""));
  if (result.error || result.status !== 0) {
    console.error(result.stdout, result.stderr, result.error || "");
    process.exit(1);
  }
  const summary = result.stdout.trim().split(/\r?\n/).filter((line) =>
    /tests passed|测试|JSON documents/.test(line)).join("\n");
  if (summary) console.log(summary);
  return result.stdout;
}
for (const suite of [
  { name: "controller", flags: ["-DARDUINO=1", "-DESP32=1", "-Itests/native", "-Iinclude"],
    files: ["src/thermal_controller.cpp", "tests/native/controller_safety_test.cpp"] },
  { name: "sensor", flags: ["-DARDUINO=1", "-DESP32=1", "-Itests/native", "-Iinclude"],
    files: ["src/tmp117_sensor.cpp", "tests/native/sensor_test.cpp"] },
  { name: "storage", flags: ["-Itests/storage", "-Iinclude"],
    files: ["src/config_store.cpp", "tests/storage/config_store_test.cpp"] },
  { name: "integration", flags: ["-Itests/integration/mocks", "-Iinclude"],
    files: ["tests/integration/api_integration_test.cpp"] },
  { name: "integration-debug", flags: ["-DPL530_ENABLE_UNSAFE_DEBUG=1", "-Itests/integration/mocks", "-Iinclude"],
    files: ["tests/integration/api_integration_test.cpp"] }
]) {
  const exe = path.join(build, `${suite.name}${process.platform === "win32" ? ".exe" : ""}`);
  run(compiler, [...flags, ...suite.flags, ...suite.files, "-o", exe], `${suite.name}-build`);
  const stdout = suite.name.startsWith("integration")
    ? run(process.execPath, ["tests/integration/verify_json.cjs", exe], suite.name)
    : run(exe, [], suite.name);
  results.push({ suite: suite.name, status: "通过", summary: stdout.trim().split(/\r?\n/).at(-1) });
}
fs.writeFileSync(path.join(root, "test-results", "native-summary.json"),
  JSON.stringify({ time: new Date().toISOString(), results }, null, 2) + "\n");
