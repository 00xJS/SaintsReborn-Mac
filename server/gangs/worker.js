// Saints Reborn gangs server: a small stand-in for the DemonWare 1.7 lobby
// ("MatchMaking Service") Saints Row's multiplayer gangs used.
// Cloudflare Worker + D1. See research/gangs-restoration.md for the protocol.
//
// The game (gangs_bridge.cpp) POSTs the DemonWare stream frames it wrote to
// /dw and gets back the frames the game should read. Frame: u16 little-endian
// length of what follows, u8 encrypted (0), u8 message type, payload.
// Payload = bdBitBuffer, bits LSB first: 1 bit "typed", then values; typed
// values carry a 5-bit type tag. Requests: message type = service (3 teams,
// 4 stats, 6 mail, 8 misc), payload [u8 slot][u8 seq][u8 task][u8 user][args].
// Reply: type 1, [u8 slot][u8 seq][u32 error][u8 task][...results].
// Message 7 = login: untyped, i32 325 + 128-byte ticket (XUID at 25, name at 33).

const T = { BOOL: 1, I8: 2, U8: 3, I16: 5, U16: 6, I32: 7, U32: 8, I64: 9, U64: 10, F32: 13, STR: 16, BLOB: 19 };
const WIDTH = { 1: 1, 2: 8, 3: 8, 5: 16, 6: 16, 7: 32, 8: 32, 9: 64, 10: 64, 13: 32 };

// DemonWare error codes the game knows (others show "Demonware gang creation failed").
const ERR = { OK: 0, GENERIC: 1, NAME_TAKEN: 301, NOT_ALLOWED: 2, NO_TEAM: 3, NOT_INVITED: 4, ALREADY: 5 };
// Member roles (u8 in member lists / memberships). Adjust here if the game shows them wrong.
const ROLE = { MEMBER: 0, ADMIN: 1, OWNER: 2 };
const MSG_TEAM_INVITE = 13;
const MAX_TEAM = 32;

// ------------------------------------------------------------ bit buffers --
export class BitReader {
  constructor(bytes) { this.b = bytes; this.p = 0; this.typed = this.bits(1) === 1; }
  left() { return this.b.length * 8 - this.p; }
  bits(n) {
    let v = 0n;
    for (let i = 0; i < n; i++) {
      if (this.p >= this.b.length * 8) throw new Error('read past end');
      const bit = (this.b[this.p >> 3] >> (this.p & 7)) & 1;
      if (bit) v |= 1n << BigInt(i);
      this.p++;
    }
    return n <= 32 ? Number(v) : v;
  }
  tag(expect) {
    if (!this.typed) return;
    const t = this.bits(5);
    if (t !== expect) throw new Error(`type ${t}, expected ${expect}`);
  }
  u8() { this.tag(T.U8); return this.bits(8); }
  bool() { this.tag(T.BOOL); return this.bits(1) === 1; }
  u32() { this.tag(T.U32); return this.bits(32) >>> 0; }
  i32() { this.tag(T.I32); return this.bits(32) | 0; }
  u64() { this.tag(T.U64); return BigInt.asUintN(64, BigInt(this.bits(64))); }
  i64() { this.tag(T.I64); return BigInt.asIntN(64, BigInt(this.bits(64))); }
  u16() { this.tag(T.U16); return this.bits(16); }
  str() {
    this.tag(T.STR);
    const out = [];
    for (;;) { const c = this.bits(8); if (c === 0) break; out.push(c); }
    return new TextDecoder().decode(new Uint8Array(out));
  }
  blob() {
    this.tag(T.BLOB);
    const n = this.u32();
    const out = new Uint8Array(n);
    for (let i = 0; i < n; i++) out[i] = this.bits(8);
    return out;
  }
  // Any typed value (for logging / unknown requests).
  any() {
    const t = this.bits(5);
    if (t === 0) return null;
    if (WIDTH[t]) { const v = this.bits(WIDTH[t]); return { t, v: typeof v === 'bigint' ? v.toString(16) : v }; }
    if (t === T.STR) { const o = []; for (;;) { const c = this.bits(8); if (!c) break; o.push(c); } return { t, v: new TextDecoder().decode(new Uint8Array(o)) }; }
    if (t === T.BLOB) { this.bits(5); const n = this.bits(32); const o = []; for (let i = 0; i < n; i++) o.push(this.bits(8)); return { t, v: hex(o) }; }
    return { t, v: '?' };
  }
}

export class BitWriter {
  constructor(typed = true) { this.b = []; this.p = 0; this.typed = typed; this.bits(typed ? 1 : 0, 1); }
  bits(v, n) {
    let x = BigInt(v);
    for (let i = 0; i < n; i++) {
      if ((this.p & 7) === 0) this.b.push(0);
      if ((x >> BigInt(i)) & 1n) this.b[this.p >> 3] |= 1 << (this.p & 7);
      this.p++;
    }
    return this;
  }
  tag(t) { if (this.typed) this.bits(t, 5); return this; }
  u8(v) { return this.tag(T.U8).bits(v & 0xff, 8); }
  bool(v) { return this.tag(T.BOOL).bits(v ? 1 : 0, 1); }
  u16(v) { return this.tag(T.U16).bits(v & 0xffff, 16); }
  u32(v) { return this.tag(T.U32).bits(BigInt.asUintN(32, BigInt(v)), 32); }
  i32(v) { return this.tag(T.I32).bits(BigInt.asUintN(32, BigInt(v)), 32); }
  u64(v) { return this.tag(T.U64).bits(BigInt.asUintN(64, BigInt(v)), 64); }
  i64(v) { return this.tag(T.I64).bits(BigInt.asUintN(64, BigInt(v)), 64); }
  str(s, max = 64) {
    this.tag(T.STR);
    const bytes = new TextEncoder().encode(s || '').slice(0, max - 1);
    for (const c of bytes) this.bits(c, 8);
    return this.bits(0, 8);
  }
  blob(bytes) { this.tag(T.BLOB).u32(bytes.length); for (const c of bytes) this.bits(c, 8); return this; }
  bytes() { return Uint8Array.from(this.b); }
}

export function frame(type, payload) {
  const len = payload.length + 2;
  const f = new Uint8Array(2 + len);
  f[0] = len & 0xff; f[1] = len >> 8; f[2] = 0; f[3] = type;
  f.set(payload, 4);
  return f;
}
export function splitFrames(body) {
  const out = [];
  let at = 0;
  while (at + 2 <= body.length) {
    const len = body[at] | (body[at + 1] << 8);
    if (at + 2 + len > body.length) break;
    if (len >= 2) out.push({ enc: body[at + 2], type: body[at + 3], payload: body.slice(at + 4, at + 2 + len) });
    at += 2 + len;
  }
  return out;
}
const hex = (a) => Array.from(a, (x) => x.toString(16).padStart(2, '0')).join('');
const xuidHex = (v) => BigInt.asUintN(64, BigInt(v)).toString(16).padStart(16, '0');
const now = () => Math.floor(Date.now() / 1000);
// The XUID the runtime's friends list gives a player (eos_lan.cpp XuidFromPuid):
// 0x0009 << 48 | low 48 bits of FNV-1a 64 over the Epic ProductUserId string.
export function xuidForPuid(puid) {
  let h = 1469598103934665603n;
  for (const c of new TextEncoder().encode(puid)) { h ^= BigInt(c); h = (h * 1099511628211n) & 0xffffffffffffffffn; }
  return xuidHex(0x0009000000000000n | (h & 0x0000ffffffffffffn));
}

// Login ticket -> { xuid, name }.
export function parseLogin(payload) {
  const r = new BitReader(payload);  // untyped: flag bit 0
  const version = r.bits(32);
  const t = new Uint8Array(128);
  for (let i = 0; i < 128; i++) t[i] = r.bits(8);
  let x = 0n;
  for (let i = 7; i >= 0; i--) x = (x << 8n) | BigInt(t[25 + i]);
  let end = 33;
  while (end < 33 + 32 && t[end]) end++;
  const name = new TextDecoder().decode(t.slice(33, end));
  return { version, xuid: xuidHex(x), name, ticket: hex(t) };
}

// ----------------------------------------------------------------- server --
export const SCHEMA = [
  `CREATE TABLE IF NOT EXISTS players (xuid TEXT PRIMARY KEY, key TEXT NOT NULL, name TEXT, created INTEGER, last_seen INTEGER, alt TEXT)`,
  `CREATE TABLE IF NOT EXISTS sessions (id TEXT PRIMARY KEY, xuid TEXT NOT NULL, created INTEGER)`,
  `CREATE TABLE IF NOT EXISTS teams (id INTEGER PRIMARY KEY AUTOINCREMENT, name TEXT NOT NULL UNIQUE COLLATE NOCASE, owner TEXT NOT NULL, created INTEGER, profile TEXT, wins INTEGER DEFAULT 0, losses INTEGER DEFAULT 0, cash INTEGER DEFAULT 0)`,
  `CREATE TABLE IF NOT EXISTS members (team_id INTEGER NOT NULL, xuid TEXT NOT NULL, role INTEGER NOT NULL, joined INTEGER, PRIMARY KEY (team_id, xuid))`,
  `CREATE TABLE IF NOT EXISTS invites (team_id INTEGER NOT NULL, xuid TEXT NOT NULL, from_xuid TEXT, created INTEGER, PRIMARY KEY (team_id, xuid))`,
  `CREATE TABLE IF NOT EXISTS messages (id INTEGER PRIMARY KEY AUTOINCREMENT, to_xuid TEXT NOT NULL, type INTEGER NOT NULL, from_xuid TEXT, from_name TEXT, team_id INTEGER, team_name TEXT, created INTEGER)`,
  `CREATE TABLE IF NOT EXISTS stats (entity TEXT NOT NULL, board INTEGER NOT NULL, data TEXT, updated INTEGER, PRIMARY KEY (entity, board))`,
  `CREATE INDEX IF NOT EXISTS players_alt ON players (alt)`,
  `CREATE TABLE IF NOT EXISTS log (id INTEGER PRIMARY KEY AUTOINCREMENT, at INTEGER, xuid TEXT, what TEXT)`,
];

async function q(db, sql, ...args) { return (await db.prepare(sql).bind(...args).all()).results || []; }
async function one(db, sql, ...args) { return await db.prepare(sql).bind(...args).first(); }
async function run(db, sql, ...args) { return await db.prepare(sql).bind(...args).run(); }

async function playerName(db, xuid) {
  const p = await one(db, 'SELECT name FROM players WHERE xuid = ?', xuid);
  return p ? p.name : '';
}
// A player named by XUID: their gang account XUID, or the friends-list XUID (alt) of one.
async function resolvePlayer(db, xuid) {
  const p = await one(db, 'SELECT xuid FROM players WHERE xuid = ? OR alt = ? ORDER BY (xuid = ?) DESC LIMIT 1', xuid, xuid, xuid);
  return p ? p.xuid : xuid;
}
async function memberRole(db, team, xuid) {
  const m = await one(db, 'SELECT role FROM members WHERE team_id = ? AND xuid = ?', team, xuid);
  return m ? m.role : null;
}
async function note(db, xuid, what) {
  await run(db, 'INSERT INTO log (at, xuid, what) VALUES (?, ?, ?)', now(), xuid, what.slice(0, 2000));
}

function reply(slot, seq, task, err, fill) {
  const w = new BitWriter(true);
  w.u8(slot).u8(seq).u32(err).u8(task);
  if (err === 0 && fill) fill(w);
  return frame(1, w.bytes());
}

// One task request -> reply frame.
async function handleTask(env, me, service, payload) {
  const db = env.DB;
  const r = new BitReader(payload);
  const slot = r.u8(), seq = r.u8(), task = r.u8();
  r.u8();  // user index
  const ok = (fill) => reply(slot, seq, task, ERR.OK, fill);
  const fail = (e) => reply(slot, seq, task, e);
  const teamRow = async (id) => one(db, 'SELECT * FROM teams WHERE id = ?', id);

  if (service === 3) {  // ---------------------------------------- teams
    switch (task) {
      case 1: {  // create team (name) -> team id
        const name = r.str().trim();
        if (!name || name.length > 32) return fail(ERR.GENERIC);
        if (await one(db, 'SELECT team_id FROM members WHERE xuid = ?', me.xuid)) return fail(ERR.ALREADY);
        if (await one(db, 'SELECT id FROM teams WHERE name = ?', name)) return fail(ERR.NAME_TAKEN);
        await run(db, 'INSERT INTO teams (name, owner, created) VALUES (?, ?, ?)', name, me.xuid, now());
        const t = await one(db, 'SELECT id FROM teams WHERE name = ?', name);
        await run(db, 'INSERT INTO members (team_id, xuid, role, joined) VALUES (?, ?, ?, ?)', t.id, me.xuid, ROLE.OWNER, now());
        await note(db, me.xuid, `create team ${t.id} "${name}"`);
        return ok((w) => w.u64(t.id));
      }
      case 20: {  // my memberships -> [team id, name, role]
        const rows = await q(db, 'SELECT t.id, t.name, m.role FROM members m JOIN teams t ON t.id = m.team_id WHERE m.xuid = ?', me.xuid);
        return ok((w) => { w.u32(rows.length); for (const x of rows) w.u64(x.id).str(x.name).u8(x.role); });
      }
      case 21: {  // members of team -> [xuid, name, online, role]
        const team = r.u64();
        const rows = await q(db, 'SELECT m.xuid, m.role, p.name, p.last_seen FROM members m LEFT JOIN players p ON p.xuid = m.xuid WHERE m.team_id = ? ORDER BY m.role DESC, m.joined', Number(team));
        const t = now();
        return ok((w) => {
          w.u32(rows.length);
          for (const x of rows) w.u64(BigInt('0x' + x.xuid)).str(x.name || '').bool(x.last_seen && t - x.last_seen < 120).u8(x.role);
        });
      }
      case 12: {  // public profile of team -> stored values (as the game wrote them)
        const team = Number(r.u64());
        const t = await teamRow(team);
        if (!t) return fail(ERR.NO_TEAM);
        const prof = t.profile ? JSON.parse(t.profile) : null;
        // The game reads u64 + three strings (823582A0 writes the strings).
        const strs = (prof || []).filter((x) => x.t === T.STR).map((x) => x.v);
        while (strs.length < 3) strs.push('');
        return ok((w) => w.u64(team).str(strs[0]).str(strs[1]).str(strs[2]));
      }
      case 16: {  // set public profile (team, values...)
        const team = Number(r.u64());
        const role = await memberRole(db, team, me.xuid);
        if (role === null || role < ROLE.ADMIN) return fail(ERR.NOT_ALLOWED);
        const vals = readRest(r);
        await run(db, 'UPDATE teams SET profile = ? WHERE id = ?', JSON.stringify(vals), team);
        await note(db, me.xuid, `profile ${team} ${JSON.stringify(vals)}`);
        return ok();
      }
      case 3:    // promote (team, member)
      case 26: { // demote (team, member)
        const team = Number(r.u64()), who = await resolvePlayer(db, xuidHex(r.u64()));
        const mine = await memberRole(db, team, me.xuid), theirs = await memberRole(db, team, who);
        if (mine === null || theirs === null || mine < ROLE.ADMIN || theirs >= mine) return fail(ERR.NOT_ALLOWED);
        if (task === 3 && mine !== ROLE.OWNER) return fail(ERR.NOT_ALLOWED);  // only the owner makes admins
        const role = task === 3 ? ROLE.ADMIN : ROLE.MEMBER;
        await run(db, 'UPDATE members SET role = ? WHERE team_id = ? AND xuid = ?', role, team, who);
        await note(db, me.xuid, `${task === 3 ? 'promote' : 'demote'} ${who} in ${team}`);
        return ok();
      }
      case 4: {  // kick (team, member)
        const team = Number(r.u64()), who = await resolvePlayer(db, xuidHex(r.u64()));
        const mine = await memberRole(db, team, me.xuid), theirs = await memberRole(db, team, who);
        if (mine === null || theirs === null || mine < ROLE.ADMIN || theirs >= mine) return fail(ERR.NOT_ALLOWED);
        await run(db, 'DELETE FROM members WHERE team_id = ? AND xuid = ?', team, who);
        await note(db, me.xuid, `kick ${who} from ${team}`);
        return ok();
      }
      case 5: {  // leave (team, successor or 0)
        const team = Number(r.u64()), next = xuidHex(r.u64());
        const mine = await memberRole(db, team, me.xuid);
        if (mine === null) return fail(ERR.NO_TEAM);
        await run(db, 'DELETE FROM members WHERE team_id = ? AND xuid = ?', team, me.xuid);
        const left = await q(db, 'SELECT xuid, role FROM members WHERE team_id = ? ORDER BY role DESC, joined', team);
        if (!left.length) {
          await run(db, 'DELETE FROM teams WHERE id = ?', team);
          await run(db, 'DELETE FROM invites WHERE team_id = ?', team);
        } else if (mine === ROLE.OWNER) {
          const heir = left.find((m) => m.xuid === next) || left[0];
          await run(db, 'UPDATE members SET role = ? WHERE team_id = ? AND xuid = ?', ROLE.OWNER, team, heir.xuid);
          await run(db, 'UPDATE teams SET owner = ? WHERE id = ?', heir.xuid, team);
        }
        await note(db, me.xuid, `leave ${team} (successor ${next})`);
        return ok();
      }
      case 6: {  // invite (team, player, blob)
        const team = Number(r.u64()), who = await resolvePlayer(db, xuidHex(r.u64()));
        if (who === me.xuid) return fail(ERR.ALREADY);
        const t = await teamRow(team);
        const mine = await memberRole(db, team, me.xuid);
        if (!t || mine === null) return fail(ERR.NOT_ALLOWED);
        if (await memberRole(db, team, who) !== null) return fail(ERR.ALREADY);
        if (await one(db, 'SELECT team_id FROM members WHERE xuid = ?', who)) return fail(ERR.ALREADY);  // in another gang
        const count = await one(db, 'SELECT COUNT(*) AS n FROM members WHERE team_id = ?', team);
        if (count.n >= MAX_TEAM) return fail(ERR.NOT_ALLOWED);
        await run(db, 'INSERT OR REPLACE INTO invites (team_id, xuid, from_xuid, created) VALUES (?, ?, ?, ?)', team, who, me.xuid, now());
        await run(db, 'DELETE FROM messages WHERE to_xuid = ? AND type = ? AND team_id = ?', who, MSG_TEAM_INVITE, team);
        await run(db, 'INSERT INTO messages (to_xuid, type, from_xuid, from_name, team_id, team_name, created) VALUES (?, ?, ?, ?, ?, ?, ?)',
          who, MSG_TEAM_INVITE, me.xuid, me.name, team, t.name, now());
        await note(db, me.xuid, `invite ${who} to ${team}`);
        return ok();
      }
      case 7:    // reject invite (team, inviter)
      case 8: {  // accept invite (team, inviter)
        const team = Number(r.u64());
        const inv = await one(db, 'SELECT * FROM invites WHERE team_id = ? AND xuid = ?', team, me.xuid);
        await run(db, 'DELETE FROM invites WHERE team_id = ? AND xuid = ?', team, me.xuid);
        if (task === 7) { await note(db, me.xuid, `decline ${team}`); return ok(); }
        if (!await teamRow(team)) return fail(ERR.NO_TEAM);
        if (!inv) return fail(ERR.NOT_INVITED);
        if (await one(db, 'SELECT team_id FROM members WHERE xuid = ?', me.xuid)) return fail(ERR.ALREADY);
        await run(db, 'INSERT INTO members (team_id, xuid, role, joined) VALUES (?, ?, ?, ?)', team, me.xuid, ROLE.MEMBER, now());
        await note(db, me.xuid, `join ${team}`);
        return ok();
      }
    }
  } else if (service === 6) {  // ------------------------------------ mail
    if (task === 1) {  // get messages (start, count, ...)
      const start = r.u32(), count = Math.min(r.u32(), 50);
      const rows = await q(db, 'SELECT * FROM messages WHERE to_xuid = ? ORDER BY id LIMIT ? OFFSET ?', me.xuid, count, start);
      return reply(slot, seq, 1, ERR.OK, (w) => {
        w.u32(rows.length);
        for (const m of rows) {
          w.u32(m.type);
          w.u64(BigInt('0x' + (m.from_xuid || '0'))).u64(m.id).u32(m.created).bool(false);  // header
          if (m.type === MSG_TEAM_INVITE) {
            w.u64(BigInt('0x' + (m.from_xuid || '0'))).str(m.from_name || '').u64(m.team_id).str(m.team_name || '');
            w.u16(0).blob(new Uint8Array(0));
          }
        }
      });
    }
    if (task === 4) {  // delete message (id)
      const id = Number(r.u64());
      await run(db, 'DELETE FROM messages WHERE id = ? AND to_xuid = ?', id, me.xuid);
      return ok();
    }
  } else if (service === 4) {  // ----------------------------------- stats
    // 1 write, 3 read by entity, 5 read range, 6 ?: accepted, nothing stored yet
    // is shown ("no statistics available"). Writes are kept for later.
    if (task === 1) {
      const vals = readRest(r);
      await note(db, me.xuid, `stats write ${JSON.stringify(vals)}`);
      return ok();
    }
    if (task === 3 || task === 5) return ok((w) => w.u32(0));
    return ok();
  } else if (service === 8) {
    return ok();
  }
  await note(db, me.xuid, `unhandled ${service}/${task} ${JSON.stringify(readRest(r))}`);
  return ok();
}

function readRest(r) {
  const vals = [];
  try {
    while (r.left() >= 5) { const v = r.any(); if (!v) break; vals.push(v); }
  } catch (e) { vals.push({ t: 0, v: 'err ' + e.message }); }
  return vals;
}
function writeValues(w, vals) {
  for (const x of vals) {
    switch (x.t) {
      case T.U64: w.u64(BigInt('0x' + x.v)); break;
      case T.I64: w.i64(BigInt.asIntN(64, BigInt('0x' + x.v))); break;
      case T.U32: w.u32(x.v); break;
      case T.I32: w.i32(x.v); break;
      case T.U8: w.u8(x.v); break;
      case T.U16: w.u16(x.v); break;
      case T.BOOL: w.bool(x.v); break;
      case T.STR: w.str(x.v); break;
    }
  }
}

export async function ensureSchema(db) {
  try { await db.prepare('ALTER TABLE players ADD COLUMN alt TEXT').run(); } catch (_) {}  // older databases
  for (const s of SCHEMA) await db.prepare(s).run();
}

// Ties the friends-list XUID of this player's Epic id to their gang account and
// moves invites that were sent to it before.
async function bindAlt(db, me, puid) {
  if (!puid) return;
  const alt = xuidForPuid(puid);
  me.alt = alt;
  const p = await one(db, 'SELECT alt FROM players WHERE xuid = ?', me.xuid);
  if (!p || p.alt === alt) return;  // already tied
  await run(db, 'UPDATE players SET alt = NULL WHERE alt = ? AND xuid != ?', alt, me.xuid);
  await run(db, 'UPDATE players SET alt = ? WHERE xuid = ?', alt, me.xuid);
  await run(db, 'UPDATE OR IGNORE invites SET xuid = ? WHERE xuid = ?', me.xuid, alt);
  await run(db, 'DELETE FROM invites WHERE xuid = ?', alt);
  await run(db, 'UPDATE messages SET to_xuid = ? WHERE to_xuid = ?', me.xuid, alt);
}

export async function handleDw(env, sessionId, key, body, puid = '') {
  const db = env.DB;
  const out = [];
  let me = null;
  const s = await one(db, 'SELECT s.xuid, p.key, p.name FROM sessions s JOIN players p ON p.xuid = s.xuid WHERE s.id = ?', sessionId);
  if (s) {
    if (s.key !== key) return { status: 403 };
    me = { xuid: s.xuid, name: s.name };
    await bindAlt(db, me, puid);
  }
  for (const f of splitFrames(body)) {
    if (f.type === 7) {  // login
      const login = parseLogin(f.payload);
      const p = await one(db, 'SELECT key FROM players WHERE xuid = ?', login.xuid);
      if (p && p.key !== key) return { status: 403 };
      if (!p) await run(db, 'INSERT INTO players (xuid, key, name, created, last_seen) VALUES (?, ?, ?, ?, ?)', login.xuid, key, login.name, now(), now());
      else await run(db, 'UPDATE players SET name = ?, last_seen = ? WHERE xuid = ?', login.name, now(), login.xuid);
      await run(db, 'INSERT OR REPLACE INTO sessions (id, xuid, created) VALUES (?, ?, ?)', sessionId, login.xuid, now());
      me = { xuid: login.xuid, name: login.name };
      await bindAlt(db, me, puid);
      continue;
    }
    if (!me) continue;  // nothing before login
    try {
      out.push(await handleTask(env, me, f.type, f.payload));
    } catch (e) {
      await note(db, me.xuid, `error type ${f.type}: ${e.message} ${hex(f.payload)}`);
      try {
        const r = new BitReader(f.payload);
        const slot = r.u8(), seq = r.u8(), task = r.u8();
        out.push(reply(slot, seq, task, ERR.GENERIC));
      } catch (_) {}
    }
  }
  if (me) {
    await run(db, 'UPDATE players SET last_seen = ? WHERE xuid = ?', now(), me.xuid);
  }
  const total = out.reduce((n, f) => n + f.length, 0);
  const res = new Uint8Array(total);
  let at = 0;
  for (const f of out) { res.set(f, at); at += f.length; }
  return { status: 200, body: res };
}

let schemaReady = false;
export default {
  async fetch(request, env) {
    const url = new URL(request.url);
    if (url.pathname === '/' || url.pathname === '/health') {
      return new Response('Saints Reborn gangs server\n', { headers: { 'content-type': 'text/plain' } });
    }
    if (url.pathname !== '/dw' || request.method !== 'POST') return new Response('not found', { status: 404 });
    const session = request.headers.get('x-sr-session') || '';
    const key = request.headers.get('x-sr-key') || '';
    if (!/^[0-9a-f]{32}$/.test(session) || !/^[0-9a-f]{32,64}$/.test(key)) return new Response('bad request', { status: 400 });
    if (!schemaReady) { await ensureSchema(env.DB); schemaReady = true; }
    const body = new Uint8Array(await request.arrayBuffer());
    if (body.length > 65536) return new Response('too big', { status: 413 });
    const puid = (request.headers.get('x-sr-puid') || '').toLowerCase();
    const r = await handleDw(env, session, key, body, /^[0-9a-f]{16,64}$/.test(puid) ? puid : '');
    if (r.status !== 200) return new Response('forbidden', { status: r.status });
    return new Response(r.body, { headers: { 'content-type': 'application/octet-stream' } });
  },
};
