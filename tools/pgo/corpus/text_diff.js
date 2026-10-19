function rng(seed) {
  let s = seed | 0 || 1;
  return function () {
    s ^= s << 13; s |= 0;
    s ^= s >>> 17;
    s ^= s << 5; s |= 0;
    return (s >>> 0) / 4294967296;
  };
}

function lcsDiff(a, b) {
  const n = a.length, m = b.length;
  const table = new Int32Array((n + 1) * (m + 1));
  for (let i = n - 1; i >= 0; i--) {
    for (let j = m - 1; j >= 0; j--) {
      const idx = i * (m + 1) + j;
      if (a[i] === b[j]) table[idx] = table[(i + 1) * (m + 1) + j + 1] + 1;
      else {
        const down = table[(i + 1) * (m + 1) + j];
        const right = table[i * (m + 1) + j + 1];
        table[idx] = down > right ? down : right;
      }
    }
  }
  const ops = [];
  let i = 0, j = 0;
  while (i < n && j < m) {
    if (a[i] === b[j]) { ops.push("="); i++; j++; }
    else if (table[(i + 1) * (m + 1) + j] >= table[i * (m + 1) + j + 1]) { ops.push("-"); i++; }
    else { ops.push("+"); j++; }
  }
  while (i < n) { ops.push("-"); i++; }
  while (j < m) { ops.push("+"); j++; }
  return ops;
}

function makeDoc(n, seed) {
  const next = rng(seed);
  const lines = [];
  for (let i = 0; i < n; i++) lines.push("line " + ((next() * 200) | 0) + " of text block " + (i % 40));
  return lines;
}

function run() {
  let adds = 0, dels = 0, same = 0;
  for (let pass = 0; pass < 2; pass++) {
    const a = makeDoc(320, pass + 1);
    const b = makeDoc(320, pass + 100);
    const ops = lcsDiff(a, b);
    for (let i = 0; i < ops.length; i++) {
      if (ops[i] === "+") adds++; else if (ops[i] === "-") dels++; else same++;
    }
  }
  return adds + ":" + dels + ":" + same;
}
print("text_diff " + run());
