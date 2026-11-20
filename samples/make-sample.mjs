// Creates samples/shop.kdb: a small shop database to try the viewer and SQL with.
//   node samples/make-sample.mjs        (replaces an existing samples/shop.kdb)
import {rmSync} from 'node:fs';
import {fileURLToPath} from 'node:url';
import {Database} from '../index.mjs';

const file = fileURLToPath(new URL('./shop.kdb', import.meta.url));
rmSync(file, {force: true});
const db = new Database(file, {create: true});

db.sql(`CREATE TABLE customers (id INTEGER PRIMARY KEY, email TEXT UNIQUE NOT NULL, name TEXT NOT NULL,
  city TEXT, plan TEXT NOT NULL DEFAULT 'free', joined DATE)`);
db.sql(`CREATE TABLE products (id INTEGER PRIMARY KEY, sku TEXT UNIQUE NOT NULL, name TEXT NOT NULL,
  price DECIMAL NOT NULL, stock INT DEFAULT 0, tags JSON)`);
db.sql(`CREATE TABLE orders (id INTEGER PRIMARY KEY, customer_id INT NOT NULL, status TEXT NOT NULL,
  total DECIMAL NOT NULL, placed DATE, items JSON)`);
db.sql('CREATE INDEX ON orders (customer_id)');
db.sql('CREATE INDEX ON orders (status)');
db.sql('CREATE INDEX ON customers (city)');

// Deterministic data (same file every time).
let seed = 42;
const rand = n => { seed = (seed * 1103515245 + 12345) % 2147483648; return seed % n; };
const pick = list => list[rand(list.length)];
const cities = ['Colombo', 'Kandy', 'Galle', 'Jaffna', 'Negombo', 'Matara'];
const first = ['Amal', 'Nimal', 'Kasun', 'Dilani', 'Sajith', 'Ishara', 'Ruwan', 'Tharushi', 'Chamara', 'Nadeesha'];
const last = ['Perera', 'Fernando', 'Silva', 'Jayasinghe', 'Bandara', 'Wijesinghe', 'Rajapaksa', 'Gunawardena'];
const products = [['TEA-01', 'Ceylon black tea 200g', 4.5, ['tea', 'food']], ['TEA-02', 'Green tea 100g', 3.9, ['tea', 'food']],
  ['SPC-01', 'Cinnamon sticks', 6.25, ['spice', 'food']], ['SPC-02', 'Curry powder', 2.75, ['spice', 'food']],
  ['CLO-01', 'Batik shirt', 24.0, ['clothing']], ['CLO-02', 'Sarong', 15.5, ['clothing']],
  ['HOM-01', 'Coconut shell bowl', 9.0, ['home']], ['HOM-02', 'Wooden elephant', 18.0, ['home', 'gift']],
  ['GFT-01', 'Gift box', 12.0, ['gift']], ['GFT-02', 'Postcards set', 3.0, ['gift', 'paper']]];
const date = (y, d) => new Date(Date.UTC(y, 0, 1 + d)).toISOString().slice(0, 10);

const p = [], v = [];
for (const [sku, name, price, tags] of products) { v.push('(?, ?, ?, ?, ?)'); p.push(sku, name, price, 20 + rand(200), tags); }
db.sql('INSERT INTO products (sku, name, price, stock, tags) VALUES ' + v.join(', '), p);

for (let b = 0; b < 1000; b += 250) {
  const vals = [], ps = [];
  for (let i = b; i < b + 250; i++) {
    const name = `${pick(first)} ${pick(last)}`;
    vals.push('(?, ?, ?, ?, ?)');
    ps.push(`customer${i + 1}@example.com`, name, pick(cities), rand(10) < 2 ? 'pro' : 'free', date(2024, rand(700)));
  }
  db.sql('INSERT INTO customers (email, name, city, plan, joined) VALUES ' + vals.join(', '), ps);
}
const statuses = ['paid', 'paid', 'paid', 'shipped', 'shipped', 'pending', 'cancelled'];
for (let b = 0; b < 5000; b += 250) {
  const vals = [], ps = [];
  for (let i = b; i < b + 250; i++) {
    const lines = Array.from({length: 1 + rand(3)}, () => { const [sku, , price] = pick(products); return {sku, qty: 1 + rand(3), price}; });
    const total = Math.round(lines.reduce((s, l) => s + l.qty * l.price, 0) * 100) / 100;
    vals.push('(?, ?, ?, ?, ?)');
    ps.push(1 + rand(1000), pick(statuses), total, date(2025, rand(320)), lines);
  }
  db.sql('INSERT INTO orders (customer_id, status, total, placed, items) VALUES ' + vals.join(', '), ps);
}
const counts = db.sql('SHOW TABLES').rows.map(t => `${t.table} ${t.rows}`).join(', ');
db.close();
console.log(`Created ${file}\n  ${counts}`);
