// Random differential SQL: queries generated from a seed run on KestroDB and SQLite
// (node:sqlite) over the same tables; results must match. The generator avoids documented,
// intentional differences: strict types (numbers are only compared with numbers, text with
// text), "/" (always decimal in KestroDB), LIKE patterns whose letters differ by case, and
// boolean values in the select list (KestroDB returns true/false, SQLite 1/0).
//   KESTRO_SQLFUZZ_SEED=123 KESTRO_SQLFUZZ_COUNT=20000 node --test tests/sqlfuzz.test.mjs
import test from 'node:test';
import assert from 'node:assert/strict';
import {DatabaseSync} from 'node:sqlite';
import {mkdtemp, rm} from 'node:fs/promises';
import {tmpdir} from 'node:os';
import {join} from 'node:path';
import {Database} from '../index.mjs';

const SEED = Number(process.env.KESTRO_SQLFUZZ_SEED ?? 20261119);
const COUNT = Number(process.env.KESTRO_SQLFUZZ_COUNT ?? 3000);

function rng(seed) { let a = seed >>> 0; return () => { a = (a + 0x6D2B79F5) >>> 0; let t = a; t = Math.imul(t ^ (t >>> 15), t | 1); t ^= t + Math.imul(t ^ (t >>> 7), t | 61); return ((t ^ (t >>> 14)) >>> 0) / 4294967296; }; }

const schema = [
  'CREATE TABLE users (id INTEGER PRIMARY KEY, name TEXT, city TEXT, age INTEGER, dept INTEGER, score REAL)',
  'CREATE TABLE orders (id INTEGER PRIMARY KEY, user INTEGER, total REAL, status TEXT, qty INTEGER)',
  'CREATE TABLE depts (id INTEGER PRIMARY KEY, title TEXT, budget INTEGER)',
];
const cities = ['Colombo', 'Kandy', 'Galle', 'Jaffna', null];
const statuses = ['paid', 'open', 'void', null];
function rows(r) {
  const out = [];
  const pick = a => a[Math.floor(r() * a.length)];
  const maybe = (v, p = 0.12) => (r() < p ? null : v);
  for (let i = 1; i <= 50; i++) out.push(['INSERT INTO users (id, name, city, age, dept, score) VALUES (?, ?, ?, ?, ?, ?)', [i, 'user' + (i % 17), pick(cities), maybe(18 + Math.floor(r() * 60)), maybe(1 + Math.floor(r() * 5)), maybe(Math.round(r() * 1000) / 10)]]);
  for (let i = 1; i <= 80; i++) out.push(['INSERT INTO orders (id, user, total, status, qty) VALUES (?, ?, ?, ?, ?)', [i, maybe(1 + Math.floor(r() * 60), 0.08), maybe(Math.round(r() * 2000) / 10), pick(statuses), maybe(Math.floor(r() * 10))]]);
  for (let i = 1; i <= 6; i++) out.push(['INSERT INTO depts (id, title, budget) VALUES (?, ?, ?)', [i, 'dept' + i, maybe(1000 * i)]]);
  return out;
}

// Columns by table and type; 'n' numbers, 't' text.
const cols = {
  users: {id: 'n', name: 't', city: 't', age: 'n', dept: 'n', score: 'n'},
  orders: {id: 'n', user: 'n', total: 'n', status: 't', qty: 'n'},
  depts: {id: 'n', title: 't', budget: 'n'},
};
const textValues = {name: ['user1', 'user5', 'user12', 'nobody'], city: ['Colombo', 'Kandy', 'Galle', 'Jaffna', 'Matara'], status: ['paid', 'open', 'void', 'x'], title: ['dept1', 'dept3', 'dept9']};
// LIKE patterns without c, k, g, j (letters that appear in upper case in the data).
const likes = ['%o%', '%a%', 'user1%', '%e%', '_a%', '%d', 'dept_', '%1', 'p%', '%n%'];

function generator(r) {
  const pick = a => a[Math.floor(r() * a.length)];
  const int = (lo, hi) => lo + Math.floor(r() * (hi - lo + 1));
  const chance = p => r() < p;
  const params = [];
  const param = v => { params.push(v); return '?' + params.length; };  // numbered: text order may differ from generation order
  const numLit = () => (chance(0.3) ? param(int(0, 100)) : String(chance(0.2) ? int(0, 100) + 0.5 : int(0, 100)));
  const textLit = c => { const v = pick(textValues[c] ?? ['x']); return chance(0.4) ? param(v) : `'${v}'`; };
  // A numeric expression over the visible columns.
  function numExpr(scope, depth = 0) {
    const ncols = scope.filter(([, , t]) => t === 'n');
    const r0 = r();
    if (depth > 1 || r0 < 0.5) { const [a, c] = pick(ncols); return `${a}.${c}`; }
    if (r0 < 0.65) return numLit();
    if (r0 < 0.8) return `(${numExpr(scope, depth + 1)} ${pick(['+', '-', '*'])} ${numExpr(scope, depth + 1)})`;
    if (r0 < 0.9) return `ABS(${numExpr(scope, depth + 1)})`;
    return `COALESCE(${numExpr(scope, depth + 1)}, ${int(0, 9)})`;
  }
  function textExpr(scope) {
    const tcols = scope.filter(([, , t]) => t === 't');
    const [a, c] = pick(tcols); const x = `${a}.${c}`;
    const r0 = r();
    if (r0 < 0.7) return x;
    if (r0 < 0.85) return `UPPER(${x})`;
    return `COALESCE(${x}, 'none')`;
  }
  function cond(scope, depth = 0) {
    const r0 = r();
    if (depth < 2 && r0 < 0.2) return `(${cond(scope, depth + 1)} ${pick(['AND', 'OR'])} ${cond(scope, depth + 1)})`;
    if (depth < 2 && r0 < 0.25) return `NOT (${cond(scope, depth + 1)})`;
    const tcols = scope.filter(([, , t]) => t === 't');
    const r1 = r();
    if (r1 < 0.35) return `${numExpr(scope)} ${pick(['=', '<>', '<', '<=', '>', '>='])} ${numExpr(scope)}`;
    if (r1 < 0.5) { const [a, c] = pick(tcols); return `${a}.${c} ${pick(['=', '<>', '<', '>'])} ${textLit(c)}`; }
    if (r1 < 0.6) { const [a, c] = pick(scope); return `${a}.${c} IS ${chance(0.5) ? 'NOT ' : ''}NULL`; }
    if (r1 < 0.7) { const e = numExpr(scope); const lo = int(0, 60); return `${e} ${chance(0.3) ? 'NOT ' : ''}BETWEEN ${lo} AND ${lo + int(0, 50)}`; }
    if (r1 < 0.8) { const [a, c] = pick(tcols); return `${a}.${c} ${chance(0.3) ? 'NOT ' : ''}IN (${Array.from({length: int(1, 3)}, () => textLit(c)).join(', ')})`; }
    if (r1 < 0.88) { const ncols = scope.filter(([, , t]) => t === 'n'); const [a, c] = pick(ncols); return `${a}.${c} ${chance(0.3) ? 'NOT ' : ''}IN (${Array.from({length: int(1, 4)}, () => (chance(0.15) ? 'NULL' : int(0, 60))).join(', ')})`; }
    if (r1 < 0.95) { const [a, c] = pick(tcols); return `${a}.${c} ${chance(0.25) ? 'NOT ' : ''}LIKE '${pick(likes)}'`; }
    // Correlated subquery on orders for a users row (alias u), or the reverse (alias o).
    if (scope.some(([a]) => a === 'u')) return `${chance(0.3) ? 'NOT ' : ''}EXISTS (SELECT 1 FROM orders x WHERE x.user = u.id AND x.qty > ${int(0, 8)})`;
    if (scope.some(([a]) => a === 'o')) return `o.user IN (SELECT y.id FROM users y WHERE y.age > ${int(20, 60)})`;
    return `1 = 1`;
  }
  function caseExpr(scope) {
    const [a, c] = pick(scope.filter(([, , t]) => t === 'n'));
    return `CASE WHEN ${cond(scope, 1)} THEN ${a}.${c} WHEN ${a}.${c} IS NULL THEN -1 ELSE ${int(0, 9)} END`;
  }

  return function query() {
    params.length = 0;
    const shape = r();
    // FROM: one table, or a join. Scope entries: [alias, column, type].
    const of = (alias, table) => Object.entries(cols[table]).map(([c, t]) => [alias, c, t]);
    let from, scopeH;
    const jr = r();
    if (jr < 0.55) { from = 'users u'; scopeH = of('u', 'users'); }
    else if (jr < 0.7) { from = 'orders o'; scopeH = of('o', 'orders'); }
    else {
      const kind = pick(['JOIN', 'LEFT JOIN', 'LEFT JOIN', 'RIGHT JOIN', 'FULL JOIN', 'JOIN']);
      if (chance(0.6)) { from = `users u ${kind} orders o ON o.user = u.id${chance(0.3) ? ` AND o.qty > ${int(0, 5)}` : ''}`; scopeH = of('u', 'users').concat(of('o', 'orders')); }
      else { from = `users u ${kind} depts d ON d.id = u.dept`; scopeH = of('u', 'users').concat(of('d', 'depts')); }
    }
    const where = chance(0.75) ? ` WHERE ${cond(scopeH)}` : '';
    if (shape < 0.3) {
      // Aggregates with GROUP BY.
      const [ga, gc] = pick(scopeH);
      const [na, nc] = pick(scopeH.filter(x => x[2] === 'n'));
      const aggs = [`COUNT(*) AS n`, `COUNT(${na}.${nc}) AS c`, `SUM(${na}.${nc}) AS s`, `MIN(${na}.${nc}) AS lo`, `MAX(${na}.${nc}) AS hi`, `AVG(${na}.${nc}) AS av`];
      const sel = aggs.filter(() => chance(0.6)); if (!sel.length) sel.push('COUNT(*) AS n');
      const grouped = chance(0.8);
      const having = grouped && chance(0.3) ? ` HAVING COUNT(*) > ${int(0, 3)}` : '';
      const sql = grouped
        ? `SELECT ${ga}.${gc} AS g, ${sel.join(', ')} FROM ${from}${where} GROUP BY ${ga}.${gc}${having}`
        : `SELECT ${sel.join(', ')} FROM ${from}${where}`;
      return {sql, params: [...params], ordered: false};
    }
    // Plain rows: a few columns or expressions, the first alias's id last for a total order.
    const first = scopeH[0][0];
    const items = [];
    for (let i = int(1, 3); i > 0; i--) {
      const r1 = r();
      if (r1 < 0.5) { const [a, c] = pick(scopeH); items.push(`${a}.${c} AS c${items.length}`); }
      else if (r1 < 0.7) items.push(`${numExpr(scopeH)} AS c${items.length}`);
      else if (r1 < 0.85) items.push(`${textExpr(scopeH)} AS c${items.length}`);
      else items.push(`${caseExpr(scopeH)} AS c${items.length}`);
    }
    const distinct = chance(0.15);
    if (!distinct) items.push(`${first}.id AS k`);
    const second = scopeH.find(x => x[0] !== first && x[1] === 'id');
    if (!distinct && second) items.push(`${second[0]}.id AS k2`);
    let sql = `SELECT ${distinct ? 'DISTINCT ' : ''}${items.join(', ')} FROM ${from}${where}`;
    let ordered = false;
    if (!distinct && chance(0.5)) {
      const keys = items.map((_, i) => (i < items.length ? `${i + 1}` : '')).filter(Boolean);
      sql += ` ORDER BY ${keys.map(k => `${k}${chance(0.3) ? ' DESC' : ''}`).join(', ')}`;
      ordered = true;
      if (chance(0.5)) sql += ` LIMIT ${int(1, 20)}${chance(0.3) ? ` OFFSET ${int(0, 10)}` : ''}`;
    }
    return {sql, params: [...params], ordered};
  };
}

const normalize = (rows, ordered) => {
  const vals = rows.map(r => JSON.stringify(Object.values(r).map(v => (typeof v === 'number' ? Math.round(v * 1e6) / 1e6 : v === undefined ? null : v))));
  return ordered ? vals : vals.sort();
};

test(`random SQL matches SQLite (seed ${SEED}, ${COUNT} queries)`, async () => {
  const dir = await mkdtemp(join(tmpdir(), 'kdb-sqlfuzz-'));
  try {
    const r = rng(SEED);
    const h = new Database(join(dir, 'h.kdb'), {create: true});
    const s = new DatabaseSync(':memory:');
    for (const q of schema) { h.sql(q); s.exec(q); }
    for (const [q, p] of rows(r)) { h.sql(q, p); s.prepare(q).run(...p); }
    for (const ix of ['CREATE INDEX ON users (city)', 'CREATE INDEX ON users (age)', 'CREATE INDEX ON orders (user)', 'CREATE INDEX ON orders (status)']) h.sql(ix);
    const next = generator(r);
    let compared = 0, bothFailed = 0;
    for (let i = 0; i < COUNT; i++) {
      const {sql, params, ordered} = next();
      let got, want, he, se;
      try { got = h.query(sql, params); } catch (e) { he = e; }
      try { want = s.prepare(sql).all(...params); } catch (e) { se = e; }
      if (he && se) { bothFailed++; continue; }
      const where = `query ${i} (seed ${SEED}): ${sql}\n  params: ${JSON.stringify(params)}`;
      assert.ok(!he, `KestroDB failed, SQLite did not: ${he?.message}\n  ${where}`);
      assert.ok(!se, `SQLite failed, KestroDB did not: ${se?.message}\n  ${where}`);
      assert.deepEqual(normalize(got, ordered), normalize(want, ordered), where);
      compared++;
    }
    assert.ok(compared > COUNT * 0.9, `only ${compared} of ${COUNT} queries compared (${bothFailed} failed on both)`);
    h.close(); s.close();
  } finally { await rm(dir, {recursive: true, force: true}); }
});
