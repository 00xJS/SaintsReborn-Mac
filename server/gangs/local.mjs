// Runs the gangs server (worker.js) on this PC for testing: http://127.0.0.1:8787
// Data in gangs_local.db next to this file. Same code as the Cloudflare Worker.
import http from 'node:http';
import { fileURLToPath } from 'node:url';
import path from 'node:path';
import worker from './worker.js';
import { makeD1 } from './d1shim.mjs';

const here = path.dirname(fileURLToPath(import.meta.url));
const env = { DB: makeD1(path.join(here, 'gangs_local.db')) };
const port = Number(process.env.PORT || 8787);

http.createServer(async (req, res) => {
  const chunks = [];
  for await (const c of req) chunks.push(c);
  const body = Buffer.concat(chunks);
  try {
    const r = await worker.fetch(new Request(`http://127.0.0.1:${port}${req.url}`, {
      method: req.method, headers: req.headers, body: req.method === 'POST' ? body : undefined,
    }), env);
    const out = Buffer.from(await r.arrayBuffer());
    res.writeHead(r.status, { 'content-type': r.headers.get('content-type') || 'application/octet-stream' });
    res.end(out);
    console.log(new Date().toISOString(), req.method, req.url, body.length, '->', r.status, out.length);
  } catch (e) {
    console.error(e);
    res.writeHead(500); res.end('error');
  }
}).listen(port, '127.0.0.1', () => console.log(`gangs server on http://127.0.0.1:${port}`));
