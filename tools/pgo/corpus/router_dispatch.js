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
// URL routing and middleware dispatch, the request path of a web framework.
// Regex compilation, string splitting, Map lookups and closure chains.
function compileRoute(pattern) {
  const names = [];
  const re = new RegExp("^" + pattern.replace(/:([\w]+)/g, function (m, n) { names.push(n); return "([^/]+)"; }) + "$");
  return { re: re, names: names, pattern: pattern };
}

function makeRouter() {
  const routes = [];
  return {
    add(pattern, handler) { const c = compileRoute(pattern); c.handler = handler; routes.push(c); },
    dispatch(url, ctx) {
      for (let i = 0; i < routes.length; i++) {
        const m = routes[i].re.exec(url);
        if (!m) continue;
        const params = {};
        for (let j = 0; j < routes[i].names.length; j++) params[routes[i].names[j]] = m[j + 1];
        return routes[i].handler(params, ctx);
      }
      return 404;
    },
  };
}

function run() {
  const router = makeRouter();
  router.add("/users/:id", function (p) { return 200 + (+p.id % 10); });
  router.add("/users/:id/posts/:postId", function (p) { return 210 + ((+p.id + +p.postId) % 10); });
  router.add("/orgs/:org/repos/:repo/issues/:num", function (p) { return 220 + (p.org.length + p.repo.length + +p.num) % 10; });
  router.add("/static/:file", function (p) { return 230 + p.file.length % 10; });
  router.add("/api/v1/search", function () { return 240; });

  const middleware = [];
  for (let i = 0; i < 6; i++) middleware.push(function (ctx, nextFn) { ctx.depth++; return nextFn(); });

  function runChain(i, ctx, final) {
    if (i === middleware.length) return final();
    return middleware[i](ctx, function () { return runChain(i + 1, ctx, final); });
  }

  const urls = [];
  for (let i = 0; i < 500; i++) {
    urls.push("/users/" + i);
    urls.push("/users/" + i + "/posts/" + (i * 3));
    urls.push("/orgs/org" + (i % 20) + "/repos/repo" + (i % 7) + "/issues/" + i);
    urls.push("/static/file" + i + ".js");
    urls.push("/api/v1/search");
    urls.push("/missing/" + i);
  }

  let total = 0;
  for (let pass = 0; pass < 8; pass++) {
    for (let i = 0; i < urls.length; i++) {
      const ctx = { depth: 0 };
      const url = urls[i];
      total = (total + runChain(0, ctx, function () { return router.dispatch(url, ctx); }) + ctx.depth) | 0;
    }
  }
  return total;
}
print("router_dispatch " + run());
