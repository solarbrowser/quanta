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
// A small Markdown to HTML converter. Line splitting, many regex passes and
// string concatenation, the classic text-pipeline shape.
function inline(s) {
  return s
    .replace(/`([^`]+)`/g, "<code>$1</code>")
    .replace(/\*\*([^*]+)\*\*/g, "<strong>$1</strong>")
    .replace(/\*([^*]+)\*/g, "<em>$1</em>")
    .replace(/\[([^\]]+)\]\(([^)]+)\)/g, '<a href="$2">$1</a>');
}

function convert(src) {
  const lines = src.split("\n");
  const out = [];
  let inList = false;
  for (let i = 0; i < lines.length; i++) {
    const line = lines[i];
    const h = /^(#{1,6})\s+(.*)$/.exec(line);
    if (h) { if (inList) { out.push("</ul>"); inList = false; }
      out.push("<h" + h[1].length + ">" + inline(h[2]) + "</h" + h[1].length + ">"); continue; }
    const li = /^[-*]\s+(.*)$/.exec(line);
    if (li) { if (!inList) { out.push("<ul>"); inList = true; } out.push("<li>" + inline(li[1]) + "</li>"); continue; }
    if (inList) { out.push("</ul>"); inList = false; }
    if (line.trim() === "") out.push("");
    else out.push("<p>" + inline(line) + "</p>");
  }
  if (inList) out.push("</ul>");
  return out.join("\n");
}

function makeDoc(sections) {
  const parts = [];
  for (let i = 0; i < sections; i++) {
    parts.push("# Section " + i);
    parts.push("Some **bold** and *italic* text with `code" + i + "` and a [link](http://x/" + i + ").");
    parts.push("");
    for (let j = 0; j < 6; j++) parts.push("- item " + j + " with **emphasis** and `tick`");
    parts.push("");
  }
  return parts.join("\n");
}

function run() {
  const doc = makeDoc(400);
  let total = 0;
  for (let pass = 0; pass < 15; pass++) total += convert(doc).length;
  return total;
}
print("markdown " + run());
