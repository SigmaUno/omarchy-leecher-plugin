// Extract the pure helper functions from BarWidget.qml and check them against
// known values. QML function bodies are plain JavaScript, so we eval the real
// ones rather than maintain a copy that could drift.
import { readFileSync } from "node:fs";
import { fileURLToPath } from "node:url";
import { dirname, join } from "node:path";

const here = dirname(fileURLToPath(import.meta.url));
const src = readFileSync(join(here, "../../BarWidget.qml"), "utf8");

function extractFunction(name) {
  const start = src.indexOf(`function ${name}(`);
  if (start < 0) throw new Error(`function ${name} not found in BarWidget.qml`);
  const open = src.indexOf("{", start);
  let depth = 0;
  for (let j = open; j < src.length; j++) {
    if (src[j] === "{") depth++;
    else if (src[j] === "}" && --depth === 0) return src.slice(start, j + 1);
  }
  throw new Error(`unbalanced braces extracting ${name}`);
}
const load = (name) => eval(`(${extractFunction(name)})`); // eslint-disable-line no-eval

let failures = 0;
let count = 0;
function check(label, got, want) {
  count++;
  const ok = JSON.stringify(got) === JSON.stringify(want);
  if (!ok) failures++;
  console.log(`  ${ok ? "ok  " : "FAIL"} ${label} = ${JSON.stringify(got)}${ok ? "" : ` (want ${JSON.stringify(want)})`}`);
}

// ---- fmt(): position/duration formatter ------------------------------------
const fmt = load("fmt");
for (const [ms, want] of [
  [0, "0:00"], [-5, "0:00"], [999, "0:00"], [7_000, "0:07"], [61_000, "1:01"],
  [331_573, "5:31"],            // the "0:31 for a 5-minute track" regression
  [599_000, "9:59"], [3_599_000, "59:59"], [3_661_000, "1:01:01"],
]) check(`fmt(${ms})`, fmt(ms), want);

// ---- enc(): control-line percent encoding (injection guard) ----------------
const enc = load("enc");
check("enc(letters)", enc("TrackName"), "TrackName");
check("enc(quote)", enc("it's"), "it%27s");
check("enc(percent)", enc("50%"), "50%25");
check("enc(newline)", enc("a\nb"), "a%0Ab");
check("enc(tab)", enc("a\tb"), "a%09b");
// every space is encoded: the transport joins fields with literal spaces and
// the backend splits on those, so a space inside a value must not survive raw
check("enc(space)", enc("a b c"), "a%20b%20c");
check("enc(del)", enc("x\x7fy"), "x%7Fy");
check("enc(control)", enc("\x01\x1f"), "%01%1F");
check("enc(unicode kept)", enc("café"), "café");

// ---- track number split (right-aligned in the library and now-playing) ----
const trackNumberOf = load("trackNumberOf");
const titleWithoutNumber = load("titleWithoutNumber");
for (const [title, num, rest] of [
  ["Banana Co. (16)", "16", "Banana Co."],
  ["The Call Of Ktulu (08)", "08", "The Call Of Ktulu"],
  // a real parenthetical must survive; only the trailing number is taken
  ["Welcome Home (Sanitarium) (08)", "08", "Welcome Home (Sanitarium)"],
  ["Welcome Home (Sanitarium)", "", "Welcome Home (Sanitarium)"],
  // the title's own leading digits are none of our business
  ["2 + 2 = 5. (The Lukewarm.) (01)", "01", "2 + 2 = 5. (The Lukewarm.)"],
  // four digits is a year, not a track number
  ["Nineteen Ninety Nine (1999)", "", "Nineteen Ninety Nine (1999)"],
  ["No number here", "", "No number here"],
  ["", "", ""],
]) {
  check(`trackNumberOf(${JSON.stringify(title)})`, trackNumberOf(title), num);
  check(`titleWithoutNumber(${JSON.stringify(title)})`, titleWithoutNumber(title), rest);
}

// ---- urlToPath(): drag-and-drop file URL -> local path --------------------
const urlToPath = load("urlToPath");
check("urlToPath(file://)", urlToPath("file:///home/me/a.flac"), "/home/me/a.flac");
check("urlToPath(file:)", urlToPath("file:/home/me/a.flac"), "/home/me/a.flac");
check("urlToPath(spaces)", urlToPath("file:///home/me/My%20Song.flac"), "/home/me/My Song.flac");
check("urlToPath(plain path)", urlToPath("/home/me/a.flac"), "/home/me/a.flac");
check("urlToPath(trailing %)", urlToPath("/a/b%"), "/a/b%");
check("urlToPath(bad escape)", urlToPath("/a/%zz"), "/a/%zz");

console.log(`\n${count} checks, ${failures} failures`);
process.exit(failures ? 1 : 0);
