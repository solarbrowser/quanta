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
// A hand-written lexer over generated source. Character-level string access,
// substring extraction and a keyword table: exactly what a parser costs.
function makeSource(n) {
  const parts = [];
  for (let i = 0; i < n; i++) {
    parts.push("function f" + i + "(a, b) { const x = a + b * " + i + "; if (x > 10) { return 'big" + i +
               "'; } else { return [x, a, b]; } }");
  }
  return parts.join("\n");
}

const KEYWORDS = { "function": 1, "const": 1, "let": 1, "var": 1, "if": 1, "else": 1, "return": 1, "for": 1, "while": 1 };

function tokenize(src) {
  const tokens = [];
  let i = 0;
  const n = src.length;
  while (i < n) {
    const c = src.charCodeAt(i);
    if (c === 32 || c === 10 || c === 9 || c === 13) { i++; continue; }
    if ((c >= 97 && c <= 122) || (c >= 65 && c <= 90) || c === 95 || c === 36) {
      const start = i;
      while (i < n) {
        const d = src.charCodeAt(i);
        if ((d >= 97 && d <= 122) || (d >= 65 && d <= 90) || (d >= 48 && d <= 57) || d === 95 || d === 36) i++;
        else break;
      }
      const word = src.slice(start, i);
      tokens.push({ kind: KEYWORDS[word] ? "keyword" : "ident", value: word, pos: start });
      continue;
    }
    if (c >= 48 && c <= 57) {
      const start = i;
      while (i < n && src.charCodeAt(i) >= 48 && src.charCodeAt(i) <= 57) i++;
      tokens.push({ kind: "number", value: src.slice(start, i), pos: start });
      continue;
    }
    if (c === 39 || c === 34) {
      const quote = c, start = i;
      i++;
      while (i < n && src.charCodeAt(i) !== quote) i++;
      i++;
      tokens.push({ kind: "string", value: src.slice(start, i), pos: start });
      continue;
    }
    tokens.push({ kind: "punct", value: src.charAt(i), pos: i });
    i++;
  }
  return tokens;
}

function run() {
  const src = makeSource(1500);
  let total = 0, idents = 0;
  for (let pass = 0; pass < 12; pass++) {
    const toks = tokenize(src);
    total += toks.length;
    for (let i = 0; i < toks.length; i++) if (toks[i].kind === "ident") idents++;
  }
  return total + ":" + idents;
}
print("tokenizer " + run());
