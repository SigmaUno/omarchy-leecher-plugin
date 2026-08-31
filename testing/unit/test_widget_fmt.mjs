// Extract fmt() from BarWidget.qml and check it against known values.
// QML function bodies are plain JavaScript, so we can eval the real one
// rather than maintain a copy.
import { readFileSync } from "node:fs";
import { fileURLToPath } from "node:url";
import { dirname, join } from "node:path";

const here = dirname(fileURLToPath(import.meta.url));
const src = readFileSync(join(here, "../../BarWidget.qml"), "utf8");

function extractFunction(name) {
  const start = src.indexOf(`function ${name}(`);
  if (start < 0) throw new Error(`function ${name} not found in BarWidget.qml`);
  let i = src.indexOf("{", start);
  let depth = 0;
  for (let j = i; j < src.length; j++) {
    if (src[j] === "{") depth++;
    else if (src[j] === "}") {
      depth--;
      if (depth === 0) return src.slice(start, j + 1);
    }
  }
  throw new Error(`unbalanced braces extracting ${name}`);
}

// eslint-disable-next-line no-eval
const fmt = eval(`(${extractFunction("fmt")})`);

const cases = [
  [0, "0:00"],
  [-5, "0:00"],
  [999, "0:00"],
  [7_000, "0:07"],
  [61_000, "1:01"],
  [331_573, "5:31"],   // "Enter Sandman" -- the 0:31 regression
  [599_000, "9:59"],
  [3_599_000, "59:59"],
  [3_661_000, "1:01:01"],
];

let failures = 0;
for (const [ms, want] of cases) {
  const got = fmt(ms);
  const ok = got === want;
  if (!ok) failures++;
  console.log(`  ${ok ? "ok  " : "FAIL"} fmt(${ms}) = ${JSON.stringify(got)}${ok ? "" : ` (want ${JSON.stringify(want)})`}`);
}

console.log(`\n${cases.length} cases, ${failures} failures`);
process.exit(failures ? 1 : 0);
