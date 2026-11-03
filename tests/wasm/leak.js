// Instances of WebAssembly modules, and the native code of their functions, are collected: the memory of the process stays where it is
// (an exported function once held its function with a root, and the instance, module and code behind it were never collected).
const bytes = new Uint8Array([0,0x61,0x73,0x6d,1,0,0,0, 1,5,1,0x60,0,1,0x7f, 3,2,1,0, 7,5,1,1,0x66,0,0, 0x0a,6,1,4,0,0x41,7,0x0b]);
function rss() { const b = new Uint8Array(readbuffer("/proc/self/status")); let t = ""; for (let i = 0; i < b.length; i++) t += String.fromCharCode(b[i]); return +/VmRSS:\s+(\d+)/.exec(t)[1]; }
function run(n) {
  for (let i = 0; i < n; i++) { const inst = new WebAssembly.Instance(new WebAssembly.Module(bytes)); inst.exports.f(); }
  gc.collect();
}
run(20000);
const a = rss();
run(20000);
const b = rss();
const grown = b - a;
console.log(grown < 10240 ? "PASS" : "FAIL", "rss grew", grown, "kB over 20000 instances (a leak is ~45000 kB)");
