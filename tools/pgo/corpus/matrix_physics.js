function rng(seed) {
  let s = seed | 0 || 1;
  return function () {
    s ^= s << 13; s |= 0;
    s ^= s >>> 17;
    s ^= s << 5; s |= 0;
    return (s >>> 0) / 4294967296;
  };
}

function mat4Identity() { return new Float64Array([1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1]); }

function mat4Multiply(out, a, b) {
  for (let i = 0; i < 4; i++) {
    for (let j = 0; j < 4; j++) {
      let s = 0;
      for (let k = 0; k < 4; k++) s += a[i * 4 + k] * b[k * 4 + j];
      out[i * 4 + j] = s;
    }
  }
  return out;
}

function run() {
  const N = 10000;
  const px = new Float64Array(N), py = new Float64Array(N), pz = new Float64Array(N);
  const vx = new Float64Array(N), vy = new Float64Array(N), vz = new Float64Array(N);
  const next = rng(99);
  for (let i = 0; i < N; i++) {
    px[i] = next() * 100; py[i] = next() * 100; pz[i] = next() * 100;
    vx[i] = next() - 0.5; vy[i] = next() - 0.5; vz[i] = next() - 0.5;
  }
  const m = mat4Identity();
  const rot = new Float64Array([0.995, -0.0998, 0, 0, 0.0998, 0.995, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1]);
  const tmp = mat4Identity();

  for (let step = 0; step < 12; step++) {
    mat4Multiply(tmp, m, rot);
    for (let i = 0; i < 16; i++) m[i] = tmp[i];
    for (let i = 0; i < N; i++) {
      vy[i] -= 0.098;
      px[i] += vx[i]; py[i] += vy[i]; pz[i] += vz[i];
      if (py[i] < 0) { py[i] = -py[i]; vy[i] = -vy[i] * 0.85; }
      const x = px[i], y = py[i], z = pz[i];
      px[i] = m[0] * x + m[1] * y + m[2] * z;
      py[i] = m[4] * x + m[5] * y + m[6] * z;
      pz[i] = m[8] * x + m[9] * y + m[10] * z;
    }
  }
  let sum = 0;
  for (let i = 0; i < N; i += 97) sum += px[i] + py[i] + pz[i];
  return sum.toFixed(4);
}
print("matrix_physics " + run());
