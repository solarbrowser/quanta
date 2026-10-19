// Seeded PRNG so every engine sees the same input. xorshift32: cheap enough
// that it never dominates the measurement.
function rng(seed) {
  let s = seed | 0 || 1;
  return function () {
    s ^= s << 13; s |= 0;
    s ^= s >>> 17;
    s ^= s << 5; s |= 0;
    return (s >>> 0) / 4294967296;
  };
}
// An LRU cache under a skewed access pattern, the shape a memoisation layer or
// a module registry has. Map insertion order, deletion and re-insertion.
function makeLRU(limit) {
  const map = new Map();
  return {
    get(k) {
      if (!map.has(k)) return undefined;
      const v = map.get(k);
      map.delete(k); map.set(k, v);
      return v;
    },
    set(k, v) {
      if (map.has(k)) map.delete(k);
      else if (map.size >= limit) map.delete(map.keys().next().value);
      map.set(k, v);
    },
    get size() { return map.size; },
  };
}

function run() {
  const cache = makeLRU(2000);
  const next = rng(7);
  let hits = 0, misses = 0;
  for (let i = 0; i < 12000; i++) {
    // Zipf-ish: most traffic lands on a small hot set.
    const r = next();
    const key = "k" + ((r * r * 8000) | 0);
    const got = cache.get(key);
    if (got === undefined) { misses++; cache.set(key, { key: key, hitCount: 0, payload: "v" + key }); }
    else { hits++; got.hitCount++; }
  }
  return hits + ":" + misses + ":" + cache.size;
}
print("lru_cache " + run());
