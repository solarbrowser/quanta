function rng(seed) {
  let s = seed | 0 || 1;
  return function () {
    s ^= s << 13; s |= 0;
    s ^= s >>> 17;
    s ^= s << 5; s |= 0;
    return (s >>> 0) / 4294967296;
  };
}

const schema = {
  type: "object",
  props: {
    id: { type: "number" },
    name: { type: "string", min: 2 },
    tags: { type: "array", of: { type: "string" } },
    meta: { type: "object", props: {
      created: { type: "number" },
      flags: { type: "array", of: { type: "boolean" } },
      nested: { type: "object", props: { depth: { type: "number" }, label: { type: "string" } } },
    } },
  },
};

function validate(value, sch, path, errors) {
  if (sch.type === "object") {
    if (value === null || typeof value !== "object" || Array.isArray(value)) {
      errors.push(path + ": expected object"); return;
    }
    for (const key in sch.props) validate(value[key], sch.props[key], path + "." + key, errors);
    return;
  }
  if (sch.type === "array") {
    if (!Array.isArray(value)) { errors.push(path + ": expected array"); return; }
    for (let i = 0; i < value.length; i++) validate(value[i], sch.of, path + "[" + i + "]", errors);
    return;
  }
  if (typeof value !== sch.type) { errors.push(path + ": expected " + sch.type); return; }
  if (sch.type === "string" && sch.min !== undefined && value.length < sch.min) {
    errors.push(path + ": too short");
  }
}

function makeDoc(i) {
  return {
    id: i,
    name: (i & 15) === 0 ? "x" : "name" + i,
    tags: ["a", "b", (i & 7) === 0 ? 5 : "c"],
    meta: {
      created: i * 1000,
      flags: [true, false, (i & 3) === 0 ? "no" : true],
      nested: { depth: i % 9, label: "L" + i },
    },
  };
}

function run() {
  let ok = 0, bad = 0, errChars = 0;
  for (let pass = 0; pass < 2; pass++) {
    for (let i = 0; i < 3000; i++) {
      const errors = [];
      validate(makeDoc(i + pass), schema, "$", errors);
      if (errors.length === 0) ok++;
      else { bad++; errChars += errors.join(";").length; }
    }
  }
  return ok + ":" + bad + ":" + errChars;
}
print("schema_validate " + run());
