// SPDX-License-Identifier: AGPL-3.0-only
// Serves the local dev build; any file it lacks (images, audio, locales, ...) is fetched
// once from pokerogue.net and cached on disk.
const http = require("http"), fs = require("fs"), path = require("path");
const [, , DIST, CACHE, PORT = "8000"] = process.argv;
const UA = "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/140.0 Safari/537.36";
const types = { ".js": "text/javascript", ".json": "application/json", ".png": "image/png", ".html": "text/html",
  ".css": "text/css", ".mp3": "audio/mpeg", ".wav": "audio/wav", ".ttf": "font/ttf", ".svg": "image/svg+xml", ".webmanifest": "application/json" };
http.createServer(async (req, res) => {
  let p = decodeURIComponent(new URL(req.url, "http://x").pathname);
  if (p.endsWith("/")) p += "index.html";
  const type = types[path.extname(p)] || "application/octet-stream";
  for (const base of [DIST, CACHE]) {
    const f = path.join(base, path.normalize(p));
    if (f.startsWith(base) && fs.existsSync(f) && fs.statSync(f).isFile()) {
      res.writeHead(200, { "content-type": type }); return fs.createReadStream(f).pipe(res);
    }
  }
  try {
    const r = await fetch("https://pokerogue.net" + p, { headers: { "user-agent": UA } });
    if (!r.ok) { res.writeHead(r.status); return res.end(); }
    const buf = Buffer.from(await r.arrayBuffer());
    const f = path.join(CACHE, path.normalize(p));
    fs.mkdirSync(path.dirname(f), { recursive: true }); fs.writeFileSync(f, buf);
    res.writeHead(200, { "content-type": type }); res.end(buf);
  } catch (e) { res.writeHead(502); res.end(String(e)); }
}).listen(+PORT, () => console.log("serving on", PORT));
