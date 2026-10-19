function rng(seed) {
  let s = seed | 0 || 1;
  return function () {
    s ^= s << 13; s |= 0;
    s ^= s >>> 17;
    s ^= s << 5; s |= 0;
    return (s >>> 0) / 4294967296;
  };
}

function makePayload(users, orders) {
  const u = [];
  for (let i = 0; i < users; i++) u.push({ id: i, name: "user" + i, region: "r" + (i % 7), active: (i & 3) !== 0 });
  const o = [];
  for (let i = 0; i < orders; i++) {
    o.push({ id: i, userId: i % users, total: ((i * 37) % 500) + 0.99, items: (i % 5) + 1, status: (i & 7) === 0 ? "void" : "ok" });
  }
  return JSON.stringify({ users: u, orders: o });
}

function run() {
  const payload = makePayload(1200, 9000);
  let checksum = 0;
  for (let pass = 0; pass < 12; pass++) {
    const data = JSON.parse(payload);
    const byId = new Map();
    for (let i = 0; i < data.users.length; i++) byId.set(data.users[i].id, data.users[i]);
    const perRegion = {};
    for (let i = 0; i < data.orders.length; i++) {
      const ord = data.orders[i];
      if (ord.status === "void") continue;
      const user = byId.get(ord.userId);
      if (!user || !user.active) continue;
      const bucket = perRegion[user.region] || (perRegion[user.region] = { region: user.region, revenue: 0, count: 0 });
      bucket.revenue += ord.total;
      bucket.count += ord.items;
    }
    const rows = Object.keys(perRegion).map(function (k) { return perRegion[k]; })
      .sort(function (a, b) { return b.revenue - a.revenue; });
    checksum = (checksum + JSON.stringify(rows).length + rows.length) | 0;
  }
  return checksum;
}
print("json_api " + run());
