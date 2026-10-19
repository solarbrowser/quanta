function rng(seed) {
  let s = seed | 0 || 1;
  return function () {
    s ^= s << 13; s |= 0;
    s ^= s >>> 17;
    s ^= s << 5; s |= 0;
    return (s >>> 0) / 4294967296;
  };
}

function compile(tpl) {
  const ops = [];
  const re = /\{\{([#\/]?)([\w.]+)\}\}/g;
  let last = 0, m;
  const stack = [ops];
  while ((m = re.exec(tpl)) !== null) {
    const cur = stack[stack.length - 1];
    if (m.index > last) { const lit = tpl.slice(last, m.index); cur.push(function () { return lit; }); }
    last = re.lastIndex;
    if (m[1] === "#") { const inner = []; const key = m[2];
      cur.push(function (ctx) {
        const list = lookup(ctx, key);
        if (!list) return "";
        let out = "";
        for (let i = 0; i < list.length; i++) out += runOps(inner, list[i]);
        return out;
      });
      stack.push(inner);
    } else if (m[1] === "/") { stack.pop(); }
    else { const key = m[2]; cur.push(function (ctx) { return String(lookup(ctx, key)); }); }
  }
  if (last < tpl.length) { const lit = tpl.slice(last); ops.push(function () { return lit; }); }
  return ops;
}

function lookup(ctx, key) {
  const parts = key.split(".");
  let v = ctx;
  for (let i = 0; i < parts.length; i++) { if (v == null) return ""; v = v[parts[i]]; }
  return v;
}

function runOps(ops, ctx) {
  let out = "";
  for (let i = 0; i < ops.length; i++) out += ops[i](ctx);
  return out;
}

function run() {
  const tpl = "<ul class='{{cls}}'>{{#rows}}<li id='{{id}}'>{{name}} - {{meta.score}}</li>{{/rows}}</ul>";
  const ops = compile(tpl);
  let total = 0;
  for (let pass = 0; pass < 400; pass++) {
    const rows = [];
    for (let i = 0; i < 120; i++) rows.push({ id: i, name: "row" + i, meta: { score: (i * pass) % 997 } });
    total += runOps(ops, { cls: "list" + pass, rows: rows }).length;
  }
  return total;
}
print("template_engine " + run());
