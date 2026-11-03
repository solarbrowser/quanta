// Runs vscode-oniguruma's onig.wasm (a real Emscripten module, an Oniguruma regex scanner) over a few hundred KB of text.
// build/bin/quanta tests/wasm/bench-onig.js [path/to/onig.wasm]
const path = (typeof ONIG !== "undefined") ? ONIG : "/usr/share/code/resources/app/node_modules.asar.unpacked/vscode-oniguruma/release/onig.wasm";
const bytes = readbuffer(path);
if (!bytes) { console.log("no onig.wasm at", path); throw new Error("missing"); }
let memory;
const heap = () => new Uint8Array(memory.buffer);
const imports = {
  env: {
    emscripten_memcpy_big: (dest, src, n) => { heap().copyWithin(dest, src, src + n); },
    emscripten_get_now: () => Date.now(),
    emscripten_resize_heap: (requested) => { try { memory.grow(Math.ceil((requested - memory.buffer.byteLength) / 65536)); return 1; } catch (e) { return 0; } },
  },
  wasi_snapshot_preview1: { fd_write: (fd, iov, iovcnt, pnum) => { new Uint32Array(memory.buffer, pnum, 1)[0] = 0; return 0; } },
};
const { instance } = { instance: new WebAssembly.Instance(new WebAssembly.Module(bytes), imports) };
const w = instance.exports;
memory = w.memory;
w.__wasm_call_ctors();
const enc = (s) => { const out = []; for (let i = 0; i < s.length; i++) out.push(s.charCodeAt(i) & 0x7f); return out; };
function put(array) { const p = w.omalloc(array.length + 1); const h = heap(); for (let i = 0; i < array.length; i++) h[p + i] = array[i]; h[p + array.length] = 0; return p; }

const patterns = ["\\bfunction\\b", "[0-9]+(\\.[0-9]+)?", "//.*$", "\"([^\"\\\\]|\\\\.)*\"", "\\b(if|else|for|while|return)\\b"];
const ptrs = patterns.map((p) => put(enc(p)));
const lens = patterns.map((p) => p.length);
const ptrArr = w.omalloc(4 * patterns.length), lenArr = w.omalloc(4 * patterns.length);
const dv = new DataView(memory.buffer);
patterns.forEach((_, i) => { dv.setUint32(ptrArr + 4 * i, ptrs[i], true); dv.setUint32(lenArr + 4 * i, lens[i], true); });
const scanner = w.createOnigScanner(ptrArr, lenArr, patterns.length, 0);
if (!scanner) { console.log("createOnigScanner failed"); throw new Error("scanner"); }
const line = 'function add(a, b) { return a + 3.14 * b; } // adds "two" numbers 42\n';
let text = ""; for (let i = 0; i < 600; i++) text += line;
const textBytes = enc(text);
const textPtr = put(textBytes);
function scanAll() {
  let pos = 0, matches = 0, last = null;
  while (pos < textBytes.length) {
    const r = w.findNextOnigScannerMatch(scanner, 1, textPtr, textBytes.length, pos, 0);
    if (!r) break;
    const view = new Uint32Array(memory.buffer, r, 4);
    const start = view[2], end = view[3];
    matches++; last = [view[0], start, end];
    pos = end > pos ? end : pos + 1;
  }
  return [matches, last];
}
const t0 = Date.now();
let result;
for (let i = 0; i < 5; i++) result = scanAll();
console.log("onig scan of", textBytes.length, "bytes x5:", Date.now() - t0, "ms; matches per scan", result[0], "last", JSON.stringify(result[1]));
