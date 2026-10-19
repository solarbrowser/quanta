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
// A signal graph with derived values and immutable state updates, the shape a
// modern front-end state library has. Heavy on closures, Map lookups and
// short-lived objects.
function createStore(initial) {
  let state = initial;
  const subs = [];
  return {
    get() { return state; },
    set(updater) { state = updater(state); for (let i = 0; i < subs.length; i++) subs[i](state); },
    subscribe(fn) { subs.push(fn); return function () { subs.splice(subs.indexOf(fn), 1); }; },
  };
}

function createSignal(compute) {
  let cached = null, dirty = true;
  return {
    invalidate() { dirty = true; },
    read(state) { if (dirty) { cached = compute(state); dirty = false; } return cached; },
  };
}

function run() {
  const items = [];
  for (let i = 0; i < 2000; i++) items.push({ id: i, done: false, weight: (i % 17) + 1, label: "item-" + i });
  const store = createStore({ items: items, filter: "all", revision: 0 });

  const totalWeight = createSignal(function (s) {
    let n = 0;
    for (let i = 0; i < s.items.length; i++) if (!s.items[i].done) n += s.items[i].weight;
    return n;
  });
  const byLabel = createSignal(function (s) {
    const m = new Map();
    for (let i = 0; i < s.items.length; i++) m.set(s.items[i].label, s.items[i]);
    return m;
  });

  store.subscribe(function () { totalWeight.invalidate(); byLabel.invalidate(); });

  let checksum = 0;
  for (let tick = 0; tick < 50; tick++) {
    store.set(function (s) {
      const nextItems = s.items.slice();
      for (let k = 0; k < 20; k++) {
        const idx = (tick * 20 + k) % nextItems.length;
        const old = nextItems[idx];
        nextItems[idx] = { id: old.id, done: !old.done, weight: old.weight, label: old.label };
      }
      return { items: nextItems, filter: s.filter, revision: s.revision + 1 };
    });
    const s = store.get();
    checksum = (checksum + totalWeight.read(s) + byLabel.read(s).size) | 0;
  }
  return checksum;
}
print("reactive_store " + run());
