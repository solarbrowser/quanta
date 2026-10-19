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
// A React-shaped workload: build a component tree, diff it against the
// previous render, and apply the patch list. Object churn, string keys, deep
// recursion and array allocation, which is what a UI framework actually costs.
function h(type, props, children) { return { type: type, props: props || {}, children: children || [] }; }

function buildTree(depth, breadth, seed) {
  const next = rng(seed);
  function node(d, path) {
    if (d === 0) return h("span", { key: path, className: "leaf-" + ((next() * 8) | 0) }, []);
    const kids = [];
    for (let i = 0; i < breadth; i++) kids.push(node(d - 1, path + "." + i));
    return h("div", { key: path, className: "n" + d, id: path, tabIndex: (next() * 100) | 0 }, kids);
  }
  return node(depth, "r");
}

function diff(a, b, patches, path) {
  if (a === undefined) { patches.push({ op: "add", path: path, type: b.type }); return; }
  if (b === undefined) { patches.push({ op: "remove", path: path }); return; }
  if (a.type !== b.type) { patches.push({ op: "replace", path: path, type: b.type }); return; }
  for (const k in b.props) {
    if (a.props[k] !== b.props[k]) patches.push({ op: "prop", path: path, key: k, value: b.props[k] });
  }
  for (const k in a.props) {
    if (!(k in b.props)) patches.push({ op: "delprop", path: path, key: k });
  }
  const n = a.children.length > b.children.length ? a.children.length : b.children.length;
  for (let i = 0; i < n; i++) diff(a.children[i], b.children[i], patches, path + "/" + i);
}

function render(node, out) {
  out.push("<" + node.type);
  const keys = Object.keys(node.props);
  for (let i = 0; i < keys.length; i++) out.push(" " + keys[i] + '="' + node.props[keys[i]] + '"');
  out.push(">");
  for (let i = 0; i < node.children.length; i++) render(node.children[i], out);
  out.push("</" + node.type + ">");
}

function run() {
  let patchCount = 0, htmlLen = 0;
  let prev = buildTree(6, 3, 1);
  for (let frame = 1; frame <= 40; frame++) {
    const cur = buildTree(6, 3, frame + 1);
    const patches = [];
    diff(prev, cur, patches, "");
    patchCount += patches.length;
    if ((frame & 7) === 0) { const out = []; render(cur, out); htmlLen += out.join("").length; }
    prev = cur;
  }
  return patchCount + ":" + htmlLen;
}
print("vdom " + run());
