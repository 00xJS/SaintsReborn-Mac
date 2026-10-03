// Minimal Cloudflare D1 API over node:sqlite, for local tests.
import { DatabaseSync } from 'node:sqlite';
export function makeD1(path = ':memory:') {
  const db = new DatabaseSync(path);
  return {
    prepare(sql) {
      let args = [];
      const st = db.prepare(sql);
      const api = {
        bind(...a) { args = a.map((x) => (typeof x === 'boolean' ? (x ? 1 : 0) : x)); return api; },
        async all() { return { results: st.all(...args) }; },
        async first() { return st.get(...args) ?? null; },
        async run() { st.run(...args); return { success: true }; },
      };
      return api;
    },
  };
}
