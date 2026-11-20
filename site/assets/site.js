/* KestroDB documentation site: shared chrome and behaviour.
 * Loaded synchronously in <head>, right after the Tailwind Play CDN script.
 * Renders the top bar, sidebar, mobile drawer, "On this page" TOC, previous/next links,
 * search, theme toggle, tabs, heading anchors and copy buttons. Each page only holds its
 * own content inside <article id="doc"> (docs) or <main id="landing"> (home page).
 * Works from file:// (no fetch, no modules). */
(function () {
  'use strict';

  /* ---------- Tailwind config (Play CDN) ---------- */
  var token = function (name) { return 'rgb(var(--c-' + name + ') / <alpha-value>)'; };
  window.tailwind = window.tailwind || {};
  window.tailwind.config = {
    darkMode: 'class',
    theme: {
      extend: {
        colors: {
          bg: token('bg'), surface: token('surface'), elev: token('elev'), line: token('line'),
          ink: token('ink'), muted: token('muted'), faint: token('faint'),
          accent: token('accent'), 'accent-soft': token('accent-soft'), 'on-accent': token('on-accent'),
          warn: token('warn'), 'warn-soft': token('warn-soft')
        },
        fontFamily: {
          sans: ['Inter', 'ui-sans-serif', 'system-ui', '-apple-system', 'Segoe UI', 'Roboto', 'sans-serif'],
          mono: ['JetBrains Mono', 'ui-monospace', 'SFMono-Regular', 'Menlo', 'Consolas', 'monospace']
        },
        maxWidth: { '8xl': '88rem' }
      }
    }
  };

  /* ---------- Theme: applied before first paint ---------- */
  var THEME_KEY = 'kestrodb-theme';
  function storedTheme() { try { return localStorage.getItem(THEME_KEY); } catch (e) { return null; } }
  function storeTheme(v) { try { localStorage.setItem(THEME_KEY, v); } catch (e) { /* private mode: ignore */ } }
  function systemDark() { try { return window.matchMedia('(prefers-color-scheme: dark)').matches; } catch (e) { return false; } }
  function applyTheme(theme) {
    var dark = theme ? theme === 'dark' : systemDark();
    document.documentElement.classList.toggle('dark', dark);
    return dark;
  }
  var forced = /[?&]theme=(dark|light)(?:&|$)/.exec(location.search);   // e.g. page.html?theme=dark (not remembered)
  function preferredTheme() { return forced ? forced[1] : storedTheme(); }
  applyTheme(preferredTheme());

  /* ---------- Site map (single source for nav, prev/next and search) ---------- */
  var script = document.currentScript;
  var ROOT = script && script.src ? new URL('../', script.src).href : './';
  var url = function (path) { return new URL(path, ROOT).href; };
  var TRACKER = url('../index.html');   // the repository's benchmark tracker page

  var GROUPS = ['Start here', 'Reference', 'Engine', 'Project'];
  var PAGES = [
    { id: 'home', group: 'Start here', title: 'Overview', href: 'index.html',
      sections: [['why', 'Why KestroDB'], ['benchmarks', 'Benchmark highlights'], ['works-with', 'Works with'], ['when-not', 'When not to use it']] },
    { id: 'getting-started', group: 'Start here', title: 'Getting started', href: 'docs/getting-started.html', sections: [] },
    { id: 'sql', group: 'Reference', title: 'SQL reference', href: 'docs/sql.html', sections: [] },
    { id: 'api', group: 'Reference', title: 'Node.js API', href: 'docs/api.html', sections: [] },
    { id: 'server', group: 'Reference', title: 'Server & clients', href: 'docs/server.html', sections: [] },
    { id: 'performance', group: 'Engine', title: 'Performance', href: 'docs/performance.html', sections: [] },
    { id: 'durability', group: 'Engine', title: 'Durability & integrity', href: 'docs/durability.html', sections: [] },
    { id: 'roadmap', group: 'Project', title: 'Roadmap', href: 'docs/roadmap.html', sections: [] }
  ];
  /* Section ids per page, used by search across pages (kept in sync with each page's h2 ids). */
  var SECTIONS = {
    'getting-started': [["requirements","Requirements"],["build","Build and test"],["cmake-options","CMake options"],["first-database","Your first database in Node.js"],["key-value","The key/value API"],["sql-tour","SQL quick tour"],["tour-tables","Create a table"],["tour-insert","Insert and upsert"],["tour-query","Query"],["tour-change","Change, index and remove"],["tour-joins","Join and use subqueries"],["tour-transactions","Transactions"],["tour-results","What comes back"],["next-steps","Next steps"]],
    'sql': [["clients","Running SQL from any client"],["placeholders","Placeholders"],["results","Results"],["errors","Errors"],["tables","Tables and rows"],["column-types","Column types"],["constraints","Constraints"],["statements","Statements"],["insert","Insert"],["upsert","Upsert"],["select","Query"],["window-examples","Window functions"],["set-examples","UNION, INTERSECT, EXCEPT"],["update-delete","Change and remove"],["join-examples","Joins"],["subquery-examples","Subqueries"],["transaction-examples","Transaction block"],["indexes","Indexes and dropping"],["alter-table","Change a table"],["expressions","Expressions"],["types","Type rules"],["transactions","Transactions"],["with","WITH and derived tables"],["with-recursive","Recursive WITH"],["with-limits","Reads, permissions and limits"],["joins","Joins and subqueries"],["join-rules","How joins run"],["subquery-rules","How subqueries run"],["window-functions","How window functions work"],["set-operations","How UNION, INTERSECT and EXCEPT combine"],["consistency","Consistency"],["speed","Indexes and speed"],["limits","Limits"],["not-supported","Not supported yet"]],
    'api': [["open","Opening a database"],["options","Open options"],["methods","Methods"],["sql","SQL"],["sql-helpers","Query helpers and prepared statements"],["sql-transactions","Transactions"],["key-value","Collections, keys and values"],["versions","Versions and optimistic concurrency"],["batches","Batches"],["async","Async writes and group commit"],["bulk","Streaming large imports"],["maintenance","Stats, verify, backup and compaction"],["backup-restore","Backups, archive and restore"],["errors","Errors"],["other-clients","Other Node.js clients"],["remote","RemoteDB (server)"],["worker","The worker process (advanced)"]],
    'server': [["run-server","Running kestro-server"],["server-options","Options"],["group-commit","Group commit across connections"],["security","Security"],["users","Users and roles"],["tls","TLS"],["protocol","Protocol overview"],["clients","Clients"],["common-api","Common API"],["client-errors","Errors"],["connections","Connections"],["transactions","Transactions per connection"],["php","PHP"],["python","Python"],["node","Node.js (RemoteDB)"],["in-process-reads","In-process reads beside the server"],["backups","Backups and restore"],["restore","Restore"],["compaction","Reclaiming space (online compaction)"],["replication","Replication"],["sync-replication","Synchronous replication"],["cluster","Cluster: automatic failover"],["embedded-python","Embedded PHP and Python"],["c-api","The C API"],["c-modes","Open modes"],["c-functions","Functions"]],
    'performance': [["node-engines","Node.js: KestroDB against four other engines"],["node-windows","Windows 10 (NTFS)"],["node-linux","Linux (Ubuntu 24.04 under WSL2)"],["node-all-engines","Every engine, every workload (Windows, 2026-11-19)"],["engine-settings","How each engine was configured"],["sql-vs-sqlite","SQL: KestroDB against SQLite"],["server-sql","SQL server against SQL server"],["vs-mariadb","kestro-server vs MariaDB"],["vs-postgresql","kestro-server vs PostgreSQL"],["php-python","PHP and Python: many processes"],["php-windows","PHP 8.2 on Windows"],["python-windows","Python 3.13 on Windows"],["through-socket","Everything through the server socket"],["capacity","Server capacity"],["linux","Linux"],["linux-vps","Every engine on a shared VPS (2026-11-15)"],["python-linux","Python 3.12 on Linux"],["php-linux","PHP 8.3 on Linux"],["capacity-linux","Server capacity on Linux"],["caveats","Caveats"],["reproduce","Reproduce"]],
    'durability': [["durable-commits","Durable commits"],["ordered-sync","Ordered sync (Linux and macOS)"],["requirements","Requirements"],["integrity","Integrity checks"],["file-format","File format"],["metadata","Metadata slots"],["groups","Transaction groups"],["publication","How a commit is published"],["read-consistency","Read consistency"],["index-snapshot","Index snapshot"],["backups","Backups and compaction"],["moving","Moving a database between systems"],["platform-status","Platform status"],["validation","What the tests cover"]],
    'roadmap': [["released","Added in 0.9.0"],["not-supported","Not supported today"],["sql-gaps","SQL"],["engine-gaps","Engine and deployment"],["validation-pending","Validation and hardening"]]
  };
  PAGES.forEach(function (p) { if (SECTIONS[p.id]) p.sections = SECTIONS[p.id]; });

  /* ---------- Small helpers ---------- */
  function h(tag, attrs, html) {
    var el = document.createElement(tag);
    if (attrs) for (var k in attrs) if (attrs[k] != null) el.setAttribute(k, attrs[k]);
    if (html != null) el.innerHTML = html;
    return el;
  }
  function esc(s) { return String(s).replace(/[&<>"']/g, function (c) { return { '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' }[c]; }); }
  function slug(s) { return s.toLowerCase().replace(/<[^>]+>/g, '').replace(/[^a-z0-9]+/g, '-').replace(/^-+|-+$/g, '').slice(0, 60) || 'section'; }

  var ICON = {
    logo: '<svg viewBox="0 0 32 32" aria-hidden="true" class="h-7 w-7"><rect width="32" height="32" rx="8" fill="rgb(var(--c-accent))"/><path d="M9 9.5v13M23 9.5v13M9 16h14" stroke="rgb(var(--c-on-accent))" stroke-width="3" stroke-linecap="round"/></svg>',
    sun: '<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.8" stroke-linecap="round" aria-hidden="true" class="h-[18px] w-[18px]"><circle cx="12" cy="12" r="4"/><path d="M12 2v2M12 20v2M4.9 4.9l1.4 1.4M17.7 17.7l1.4 1.4M2 12h2M20 12h2M4.9 19.1l1.4-1.4M17.7 6.3l1.4-1.4"/></svg>',
    moon: '<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.8" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true" class="h-[18px] w-[18px]"><path d="M21 12.8A9 9 0 1 1 11.2 3a7 7 0 0 0 9.8 9.8z"/></svg>',
    menu: '<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.8" stroke-linecap="round" aria-hidden="true" class="h-5 w-5"><path d="M4 7h16M4 12h16M4 17h16"/></svg>',
    close: '<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.8" stroke-linecap="round" aria-hidden="true" class="h-5 w-5"><path d="M6 6l12 12M18 6L6 18"/></svg>',
    search: '<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.8" stroke-linecap="round" aria-hidden="true" class="h-4 w-4"><circle cx="11" cy="11" r="7"/><path d="M20 20l-3.5-3.5"/></svg>',
    copy: '<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.8" aria-hidden="true" class="h-3.5 w-3.5"><rect x="9" y="9" width="11" height="11" rx="2"/><path d="M5 15V5a2 2 0 0 1 2-2h10"/></svg>',
    check: '<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true" class="h-3.5 w-3.5"><path d="M5 12l5 5L20 7"/></svg>',
    ext: '<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.8" stroke-linecap="round" aria-hidden="true" class="h-3.5 w-3.5"><path d="M14 4h6v6M20 4l-9 9M18 14v5a1 1 0 0 1-1 1H5a1 1 0 0 1-1-1V7a1 1 0 0 1 1-1h5"/></svg>',
    left: '<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.8" stroke-linecap="round" aria-hidden="true" class="h-4 w-4"><path d="M15 6l-6 6 6 6"/></svg>',
    right: '<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.8" stroke-linecap="round" aria-hidden="true" class="h-4 w-4"><path d="M9 6l6 6-6 6"/></svg>'
  };

  /* ---------- Top bar ---------- */
  function topBar(current) {
    var nav = [
      { label: 'Docs', href: url('docs/getting-started.html'), active: current !== 'home' && current !== 'performance' },
      { label: 'SQL', href: url('docs/sql.html'), active: current === 'sql' },
      { label: 'Performance', href: url('docs/performance.html'), active: current === 'performance' },
      { label: 'Benchmark tracker', href: TRACKER, active: false }
    ];
    var links = nav.map(function (n) {
      return '<a href="' + n.href + '" class="rounded-md px-3 py-1.5 text-sm font-medium transition-colors ' +
        (n.active ? 'text-ink bg-surface' : 'text-muted hover:text-ink') + '"' + (n.active ? ' aria-current="true"' : '') + '>' + n.label + '</a>';
    }).join('');
    var header = h('header', { id: 'topbar', class: 'sticky top-0 z-40 border-b border-line bg-bg/85 backdrop-blur supports-[backdrop-filter]:bg-bg/75' });
    header.innerHTML =
      '<div class="mx-auto flex h-16 max-w-8xl items-center gap-3 px-4 sm:px-6">' +
        '<button type="button" id="menu-btn" class="-ml-1.5 inline-flex h-9 w-9 items-center justify-center rounded-md text-muted hover:text-ink hover:bg-surface lg:hidden" aria-label="Open navigation" aria-expanded="false" aria-controls="drawer">' + ICON.menu + '</button>' +
        '<a href="' + url('index.html') + '" class="flex items-center gap-2.5 font-semibold tracking-tight text-ink" aria-label="KestroDB home">' + ICON.logo +
          '<span class="text-[1.05rem]">KestroDB</span><span class="hidden rounded-full border border-line px-2 py-0.5 text-[11px] font-medium text-faint sm:inline">0.9</span></a>' +
        '<nav aria-label="Main" class="ml-4 hidden items-center gap-1 md:flex">' + links + '</nav>' +
        '<div class="ml-auto flex items-center gap-2">' +
          '<div class="relative hidden w-56 md:block lg:w-64" data-search-host>' +
            '<label for="search-top" class="sr-only">Search the docs</label>' +
            '<span class="pointer-events-none absolute left-3 top-1/2 -translate-y-1/2 text-faint">' + ICON.search + '</span>' +
            '<input id="search-top" type="search" autocomplete="off" spellcheck="false" placeholder="Search docs" ' +
              'class="h-9 w-full rounded-lg border border-line bg-surface pl-9 pr-10 text-sm text-ink placeholder:text-faint focus:border-accent focus:outline-none focus:ring-2 focus:ring-accent/25" ' +
              'role="combobox" aria-expanded="false" aria-autocomplete="list" aria-controls="search-top-results">' +
            '<kbd class="k pointer-events-none absolute right-2.5 top-1/2 -translate-y-1/2">/</kbd>' +
          '</div>' +
          '<button type="button" id="theme-btn" class="inline-flex h-9 w-9 items-center justify-center rounded-md border border-line text-muted hover:text-ink hover:bg-surface"></button>' +
        '</div>' +
      '</div>';
    return header;
  }

  /* ---------- Sidebar nav (desktop and drawer share the markup) ---------- */
  function sideNav(current, idSuffix) {
    var out = '<nav aria-label="Documentation' + (idSuffix ? ' (menu)' : '') + '" class="space-y-6">';
    GROUPS.forEach(function (g) {
      var items = PAGES.filter(function (p) { return p.group === g; });
      out += '<div><p class="mb-2 px-3 text-[11px] font-semibold uppercase tracking-[0.08em] text-faint" id="grp-' + slug(g) + idSuffix + '">' + g + '</p>' +
        '<ul class="space-y-0.5" aria-labelledby="grp-' + slug(g) + idSuffix + '">';
      items.forEach(function (p) {
        out += '<li><a class="side-link" href="' + url(p.href) + '"' + (p.id === current ? ' aria-current="page"' : '') + '>' + esc(p.title) + '</a></li>';
      });
      out += '</ul></div>';
    });
    out += '<div><p class="mb-2 px-3 text-[11px] font-semibold uppercase tracking-[0.08em] text-faint">Data</p><ul><li>' +
      '<a class="side-link" href="' + TRACKER + '">Benchmark tracker <span class="text-faint">' + ICON.ext + '</span></a></li></ul></div>';
    return out + '</nav>';
  }

  function footer() {
    var f = h('footer', { class: 'border-t border-line' });
    f.innerHTML = '<div class="mx-auto flex max-w-8xl flex-col gap-3 px-4 py-8 text-sm text-faint sm:flex-row sm:items-center sm:justify-between sm:px-6">' +
      '<p>KestroDB 0.9.0 documentation. The numbers on this site come from the benchmark records in <code class="inline-code">results/</code> and from <code class="inline-code">VALIDATION.md</code>. Apache License 2.0.</p>' +
      '<p class="flex gap-4"><a class="hover:text-ink" href="' + url('docs/getting-started.html') + '">Docs</a><a class="hover:text-ink" href="' + url('docs/performance.html') + '">Performance</a><a class="hover:text-ink" href="' + TRACKER + '">Tracker</a><a class="hover:text-ink" href="https://github.com/HimasRafeek/kestrodb">GitHub</a></p></div>';
    return f;
  }

  /* ---------- Layouts ---------- */
  function buildDocs(current) {
    var article = document.getElementById('doc');
    var body = document.body;
    body.insertBefore(h('a', { href: '#doc', class: 'skip-link' }, 'Skip to content'), body.firstChild);
    body.insertBefore(topBar(current), article);

    var shell = h('div', { class: 'mx-auto flex max-w-8xl' });
    var side = h('aside', { id: 'sidebar', class: 'hidden w-64 shrink-0 border-r border-line lg:block' });
    side.innerHTML = '<div class="sticky top-16 h-[calc(100vh-4rem)] overflow-y-auto px-4 py-8">' + sideNav(current, '') + '</div>';
    var main = h('main', { id: 'main', class: 'min-w-0 flex-1 px-4 pb-16 pt-8 sm:px-8 lg:pt-10 xl:px-12' });
    var inner = h('div', { class: 'mx-auto max-w-[46rem]' });
    var tocAside = h('aside', { id: 'toc', class: 'hidden w-64 shrink-0 xl:block', 'aria-label': 'On this page' });
    body.insertBefore(shell, article);
    shell.appendChild(side); shell.appendChild(main); shell.appendChild(tocAside);
    main.appendChild(inner); inner.appendChild(article);
    article.classList.add('doc');
    article.setAttribute('tabindex', '-1');
    inner.appendChild(prevNext(current));
    body.appendChild(footer());
    body.appendChild(drawer(current));
    enhanceHeadings(article);
    buildToc(article, tocAside);
  }

  function buildLanding() {
    var landing = document.getElementById('landing');
    var body = document.body;
    body.insertBefore(h('a', { href: '#landing', class: 'skip-link' }, 'Skip to content'), body.firstChild);
    body.insertBefore(topBar('home'), landing);
    landing.setAttribute('tabindex', '-1');
    body.appendChild(footer());
    body.appendChild(drawer('home'));
  }

  function prevNext(current) {
    var i = PAGES.findIndex(function (p) { return p.id === current; });
    var nav = h('nav', { 'aria-label': 'Previous and next page', class: 'mt-16 grid gap-4 border-t border-line pt-8 sm:grid-cols-2' });
    var card = function (p, dir) {
      if (!p) return '<span class="hidden sm:block"></span>';
      return '<a href="' + url(p.href) + '" rel="' + (dir === 'prev' ? 'prev' : 'next') + '" class="group rounded-xl border border-line p-4 transition-colors hover:border-accent/60 ' + (dir === 'next' ? 'sm:text-right' : '') + '">' +
        '<span class="flex items-center gap-1 text-xs font-medium text-faint ' + (dir === 'next' ? 'sm:justify-end' : '') + '">' + (dir === 'prev' ? ICON.left + 'Previous' : 'Next' + ICON.right) + '</span>' +
        '<span class="mt-1 block font-semibold text-ink group-hover:text-accent">' + esc(p.title) + '</span></a>';
    };
    nav.innerHTML = card(PAGES[i - 1], 'prev') + card(PAGES[i + 1], 'next');
    return nav;
  }

  /* ---------- Mobile drawer ---------- */
  function drawer(current) {
    var d = h('div', { id: 'drawer', class: 'fixed inset-0 z-50 lg:hidden', hidden: '' });
    d.innerHTML = '<div class="absolute inset-0 bg-black/40" data-close></div>' +
      '<div class="absolute inset-y-0 left-0 flex w-[min(20rem,88vw)] flex-col border-r border-line bg-bg shadow-2xl" role="dialog" aria-modal="true" aria-label="Navigation">' +
        '<div class="flex h-16 items-center justify-between border-b border-line px-4">' +
          '<a href="' + url('index.html') + '" class="flex items-center gap-2.5 font-semibold text-ink">' + ICON.logo + 'KestroDB</a>' +
          '<button type="button" class="inline-flex h-9 w-9 items-center justify-center rounded-md text-muted hover:bg-surface hover:text-ink" aria-label="Close navigation" data-close>' + ICON.close + '</button>' +
        '</div>' +
        '<div class="flex-1 overflow-y-auto px-4 py-5">' +
          '<div class="relative mb-6" data-search-host>' +
            '<label for="search-drawer" class="sr-only">Search the docs</label>' +
            '<span class="pointer-events-none absolute left-3 top-1/2 -translate-y-1/2 text-faint">' + ICON.search + '</span>' +
            '<input id="search-drawer" type="search" autocomplete="off" spellcheck="false" placeholder="Search docs" role="combobox" aria-expanded="false" aria-autocomplete="list" aria-controls="search-drawer-results" ' +
              'class="h-10 w-full rounded-lg border border-line bg-surface pl-9 pr-3 text-sm text-ink placeholder:text-faint focus:border-accent focus:outline-none focus:ring-2 focus:ring-accent/25">' +
          '</div>' +
          '<ul class="mb-6 space-y-0.5 border-b border-line pb-6 md:hidden">' +
            '<li><a class="side-link" href="' + url('docs/performance.html') + '">Performance</a></li>' +
            '<li><a class="side-link" href="' + TRACKER + '">Benchmark tracker</a></li></ul>' +
          sideNav(current, '-m') +
        '</div>' +
      '</div>';
    return d;
  }

  function wireDrawer() {
    var btn = document.getElementById('menu-btn'), d = document.getElementById('drawer');
    if (!btn || !d) return;
    function open() {
      d.hidden = false; btn.setAttribute('aria-expanded', 'true'); document.body.style.overflow = 'hidden';
      var first = d.querySelector('input, a'); if (first) first.focus();
    }
    function close() {
      if (d.hidden) return;
      d.hidden = true; btn.setAttribute('aria-expanded', 'false'); document.body.style.overflow = ''; btn.focus();
    }
    btn.addEventListener('click', open);
    d.addEventListener('click', function (e) {
      if (e.target.closest('[data-close]')) close();
      else if (e.target.closest('a')) { d.hidden = true; btn.setAttribute('aria-expanded', 'false'); document.body.style.overflow = ''; }
    });
    d.addEventListener('keydown', function (e) {
      if (e.key === 'Escape') { close(); return; }
      if (e.key !== 'Tab') return;
      var f = d.querySelectorAll('a, button, input');
      var first = f[0], last = f[f.length - 1];
      if (e.shiftKey && document.activeElement === first) { e.preventDefault(); last.focus(); }
      else if (!e.shiftKey && document.activeElement === last) { e.preventDefault(); first.focus(); }
    });
    window.addEventListener('resize', function () { if (window.innerWidth >= 1024) close(); });
  }

  /* ---------- Theme toggle ---------- */
  function wireTheme() {
    var b = document.getElementById('theme-btn');
    if (!b) return;
    function paint() {
      var dark = document.documentElement.classList.contains('dark');
      b.innerHTML = dark ? ICON.sun : ICON.moon;
      b.setAttribute('aria-label', dark ? 'Switch to light theme' : 'Switch to dark theme');
      b.setAttribute('title', dark ? 'Light theme' : 'Dark theme');
    }
    b.addEventListener('click', function () {
      var dark = !document.documentElement.classList.contains('dark');
      forced = null; applyTheme(dark ? 'dark' : 'light'); storeTheme(dark ? 'dark' : 'light'); paint();
    });
    try {
      window.matchMedia('(prefers-color-scheme: dark)').addEventListener('change', function () {
        if (!preferredTheme()) { applyTheme(null); paint(); }
      });
    } catch (e) { /* old browsers */ }
    paint();
  }

  /* ---------- Headings, TOC ---------- */
  function enhanceHeadings(root) {
    var used = {};
    root.querySelectorAll('h2, h3').forEach(function (hd) {
      if (hd.closest('.card, .no-toc')) return;
      if (!hd.id) { var s = slug(hd.textContent), id = s, n = 2; while (used[id] || document.getElementById(id)) id = s + '-' + n++; hd.id = id; }
      used[hd.id] = true;
      if (!hd.querySelector('.anchor')) hd.appendChild(h('a', { class: 'anchor', href: '#' + hd.id, 'aria-label': 'Link to section: ' + hd.textContent.trim() }, '#'));
    });
  }
  function headingText(hd) { var c = hd.cloneNode(true); c.querySelectorAll('.anchor').forEach(function (a) { a.remove(); }); return c.textContent.trim(); }

  function buildToc(article, aside) {
    var heads = Array.prototype.slice.call(article.querySelectorAll('h2, h3')).filter(function (x) { return !x.closest('.card, .no-toc'); });
    if (heads.length < 3) return;
    var list = heads.map(function (hd) {
      return '<li><a class="toc-link' + (hd.tagName === 'H3' ? ' lvl3' : '') + '" href="#' + hd.id + '" data-target="' + hd.id + '">' + esc(headingText(hd)) + '</a></li>';
    }).join('');
    aside.innerHTML = '<div class="sticky top-16 max-h-[calc(100vh-4rem)] overflow-y-auto py-10 pl-2 pr-6">' +
      '<p class="mb-3 text-[11px] font-semibold uppercase tracking-[0.08em] text-faint">On this page</p><ul>' + list + '</ul>' +
      '<a href="#doc" class="mt-6 inline-block text-xs text-faint hover:text-ink">Back to top ↑</a></div>';
    var inline = h('details', { class: 'toc-inline' });
    inline.innerHTML = '<summary>On this page</summary><ul>' + heads.filter(function (x) { return x.tagName === 'H2'; }).map(function (hd) {
      return '<li><a href="#' + hd.id + '">' + esc(headingText(hd)) + '</a></li>';
    }).join('') + '</ul>';
    var head = article.querySelector('.doc-head');
    if (head) head.insertAdjacentElement('afterend', inline);

    var links = aside.querySelectorAll('.toc-link');
    var byId = {}; links.forEach(function (l) { byId[l.dataset.target] = l; });
    function update() {
      var y = window.scrollY + 110, active = heads[0];
      for (var i = 0; i < heads.length; i++) { if (heads[i].getBoundingClientRect().top + window.scrollY <= y) active = heads[i]; else break; }
      if (window.innerHeight + window.scrollY >= document.documentElement.scrollHeight - 4) active = heads[heads.length - 1];
      links.forEach(function (l) { l.classList.remove('active'); l.removeAttribute('aria-current'); });
      var l = byId[active.id]; if (l) { l.classList.add('active'); l.setAttribute('aria-current', 'location'); }
    }
    var ticking = false;
    window.addEventListener('scroll', function () { if (!ticking) { ticking = true; requestAnimationFrame(function () { ticking = false; update(); }); } }, { passive: true });
    update();
  }

  /* ---------- Search ---------- */
  function searchIndex(current) {
    var items = [];
    PAGES.forEach(function (p) {
      items.push({ title: p.title, where: p.group, href: url(p.href), page: true });
      if (p.id === current) return;   // current page: use its live headings instead
      p.sections.forEach(function (s) { items.push({ title: s[1], where: p.title, href: url(p.href) + '#' + s[0] }); });
    });
    var cur = PAGES.filter(function (p) { return p.id === current; })[0];
    var root = document.getElementById('doc') || document.getElementById('landing');
    if (root && cur) root.querySelectorAll('h2[id], h3[id]').forEach(function (hd) {
      items.push({ title: headingText(hd), where: cur.title + ' (this page)', href: '#' + hd.id });
    });
    return items;
  }

  function wireSearch(current) {
    var items = null;
    document.querySelectorAll('[data-search-host]').forEach(function (host) {
      var input = host.querySelector('input');
      var panel = h('div', { id: input.id + '-results', class: 'search-panel', role: 'listbox', hidden: '' });
      host.appendChild(panel);
      var sel = -1, shown = [];
      function render() {
        items = items || searchIndex(current);
        var q = input.value.trim().toLowerCase();
        if (!q) { panel.hidden = true; input.setAttribute('aria-expanded', 'false'); return; }
        var terms = q.split(/\s+/);
        shown = items.filter(function (it) {
          var hay = (it.title + ' ' + it.where).toLowerCase();
          return terms.every(function (t) { return hay.indexOf(t) >= 0; });
        }).sort(function (a, b) {
          var at = a.title.toLowerCase().indexOf(terms[0]), bt = b.title.toLowerCase().indexOf(terms[0]);
          return (at < 0 ? 99 : at) - (bt < 0 ? 99 : bt) || (b.page ? 1 : 0) - (a.page ? 1 : 0);
        }).slice(0, 12);
        sel = shown.length ? 0 : -1;
        if (!shown.length) panel.innerHTML = '<p class="search-empty">No matches for “' + esc(input.value.trim()) + '”.</p>';
        else panel.innerHTML = shown.map(function (it, i) {
          var t = esc(it.title);
          terms.forEach(function (term) { if (term.length > 1) t = t.replace(new RegExp('(' + term.replace(/[.*+?^${}()|[\]\\]/g, '\\$&') + ')', 'ig'), '<mark>$1</mark>'); });
          return '<a class="search-item" role="option" id="' + input.id + '-opt-' + i + '" href="' + it.href + '" aria-selected="' + (i === sel) + '">' + t + '<small>' + esc(it.where) + '</small></a>';
        }).join('');
        panel.hidden = false; input.setAttribute('aria-expanded', 'true');
        mark();
      }
      function mark() {
        panel.querySelectorAll('.search-item').forEach(function (a, i) { a.setAttribute('aria-selected', String(i === sel)); if (i === sel) a.scrollIntoView({ block: 'nearest' }); });
        if (sel >= 0) input.setAttribute('aria-activedescendant', input.id + '-opt-' + sel); else input.removeAttribute('aria-activedescendant');
      }
      input.addEventListener('input', render);
      input.addEventListener('focus', function () { if (input.value.trim()) render(); });
      input.addEventListener('keydown', function (e) {
        if (e.key === 'ArrowDown' && shown.length) { e.preventDefault(); sel = (sel + 1) % shown.length; mark(); }
        else if (e.key === 'ArrowUp' && shown.length) { e.preventDefault(); sel = (sel - 1 + shown.length) % shown.length; mark(); }
        else if (e.key === 'Enter' && sel >= 0 && !panel.hidden) { e.preventDefault(); go(shown[sel].href); }
        else if (e.key === 'Escape') { if (input.value) { input.value = ''; render(); } else input.blur(); }
      });
      panel.addEventListener('click', function (e) { var a = e.target.closest('a'); if (a) { panel.hidden = true; input.value = ''; } });
      document.addEventListener('click', function (e) { if (!host.contains(e.target)) { panel.hidden = true; input.setAttribute('aria-expanded', 'false'); } });
      function go(href) {
        panel.hidden = true; input.value = ''; input.setAttribute('aria-expanded', 'false');
        var d = document.getElementById('drawer'); if (d && !d.hidden && d.contains(input)) { d.hidden = true; document.body.style.overflow = ''; }
        window.location.href = href;
        if (href.charAt(0) === '#') { var t = document.getElementById(href.slice(1)); if (t) { t.setAttribute('tabindex', '-1'); t.focus({ preventScroll: true }); } }
      }
    });
    document.addEventListener('keydown', function (e) {
      if (e.key !== '/' || e.ctrlKey || e.metaKey || e.altKey) return;
      var t = e.target; if (t && (t.tagName === 'INPUT' || t.tagName === 'TEXTAREA' || t.isContentEditable)) return;
      var top = document.getElementById('search-top');
      if (top && top.offsetParent !== null) { e.preventDefault(); top.focus(); }
      else { var b = document.getElementById('menu-btn'); if (b) { e.preventDefault(); b.click(); } }
    });
  }

  /* ---------- Code blocks: highlighting + copy ---------- */
  var KW = {
    js: 'import|from|export|const|let|var|new|await|async|for|of|in|try|finally|catch|return|true|false|null|undefined|function|yield|if|else|class|static|using|throw',
    sql: 'SELECT|FROM|WHERE|AND|OR|NOT|IN|BETWEEN|LIKE|ILIKE|IS|NULL|ORDER|BY|ASC|DESC|LIMIT|OFFSET|GROUP|HAVING|DISTINCT|AS|INSERT|INTO|VALUES|UPDATE|SET|DELETE|CREATE|DROP|TABLE|INDEX|UNIQUE|ON|PRIMARY|KEY|INTEGER|INT|TEXT|VARCHAR|CONFLICT|DO|NOTHING|DUPLICATE|IGNORE|REPLACE|COUNT|SUM|AVG|MIN|MAX|LOWER|UPPER|LENGTH|ABS|ROUND|COALESCE|IFNULL|DEFAULT|CHECK|JOIN|INNER|LEFT|RIGHT|FULL|OUTER|CROSS|EXISTS|ROLLBACK|UNION|USING|BEGIN|COMMIT|ALTER|ADD|COLUMN|RENAME|TO|BOOLEAN|REAL|JSON',
    python: 'import|from|def|class|return|None|True|False|with|as|for|in|if|else|try|except|finally|raise|not|and|or|lambda|while|print',
    php: 'require|new|function|null|true|false|return|use|namespace|foreach|as|if|else|try|catch|throw|array|getenv|echo|var_dump',
    c: 'typedef|struct|const|char|int|size_t|uint64_t|int64_t|uint32_t|void|enum|return|if|else|unsigned|include|define|NULL|printf|while|for|static',
    sh: 'npm|node|cmake|ctest|cd|echo|python3|php|g\\+\\+'
  };
  function highlight(code, lang) {
    var L = { javascript: 'js', mjs: 'js', js: 'js', sql: 'sql', python: 'python', py: 'python', php: 'php', c: 'c', h: 'c', sh: 'sh', bash: 'sh', text: 'sh', shell: 'sh' }[lang];
    if (!L) return esc(code);
    var comment = { js: '\\/\\/[^\\n]*', sql: '--[^\\n]*', python: '#[^\\n]*', php: '\\/\\/[^\\n]*|\\/\\*[\\s\\S]*?\\*\\/', c: '\\/\\*[\\s\\S]*?\\*\\/|\\/\\/[^\\n]*', sh: '(?:^|(?<=\\s))#[^\\n]*' }[L];
    var str = L === 'sh' ? '"[^"\\n]*"' : (L === 'js' ? '`[^`]*`|' : '') + '"(?:[^"\\\\\\n]|\\\\.)*"|\'(?:[^\'\\\\\\n]|\\\\.)*\'';
    var flags = L === 'sql' ? 'gi' : 'g';
    var parts = ['(' + comment + ')', '(' + str + ')', '(\\b\\d[\\d_.,]*\\b)', '(\\b(?:' + KW[L] + ')\\b)'];
    if (L === 'php') parts.push('(\\$[A-Za-z_]\\w*)');
    parts.push('(\\b[A-Za-z_]\\w*(?=\\())');
    var re;
    try { re = new RegExp(parts.join('|'), flags + 'm'); } catch (e) { return esc(code); }
    var out = '', last = 0, m;
    while ((m = re.exec(code))) {
      if (m[0] === '') { re.lastIndex++; continue; }
      out += esc(code.slice(last, m.index));
      var cls = m[1] ? 'c' : m[2] ? 's' : m[3] ? 'n' : m[4] ? 'k' : (L === 'php' && m[5]) ? 'v' : 'f';
      if (cls === 'k' && L === 'sql' && m[0] !== m[0].toUpperCase()) cls = null;   // only uppercase SQL keywords
      out += cls ? '<span class="tok-' + cls + '">' + esc(m[0]) + '</span>' : esc(m[0]);
      last = m.index + m[0].length;
    }
    return out + esc(code.slice(last));
  }

  function copyText(text) {
    if (navigator.clipboard && window.isSecureContext) return navigator.clipboard.writeText(text);
    return new Promise(function (resolve, reject) {
      var ta = h('textarea', { 'aria-hidden': 'true', style: 'position:fixed;left:-9999px;top:0' });
      ta.value = text; document.body.appendChild(ta); ta.select();
      try { document.execCommand('copy') ? resolve() : reject(); } catch (e) { reject(e); } finally { ta.remove(); }
    });
  }

  function enhanceCode(root) {
    root.querySelectorAll('pre > code').forEach(function (codeEl) {
      var pre = codeEl.parentElement;
      if (pre.closest('.code')) return;
      var m = /language-([\w-]+)/.exec(codeEl.className), lang = m ? m[1] : 'text';
      var raw = codeEl.textContent.replace(/^\n/, '').replace(/\s+$/, '');
      codeEl.innerHTML = highlight(raw, lang);
      var label = pre.getAttribute('data-title') || { js: 'JavaScript', javascript: 'JavaScript', sql: 'SQL', python: 'Python', php: 'PHP', c: 'C', sh: 'Terminal', text: 'Terminal', bash: 'Terminal' }[lang] || lang;
      var wrap = h('div', { class: 'code' });
      var head = h('div', { class: 'code-head' }, '<span>' + esc(label) + '</span>');
      var btn = h('button', { type: 'button', class: 'copy-btn', 'aria-label': 'Copy code to clipboard' }, ICON.copy + '<span>Copy</span>');
      btn.addEventListener('click', function () {
        copyText(raw).then(function () {
          btn.setAttribute('data-copied', ''); btn.innerHTML = ICON.check + '<span>Copied</span>';
          setTimeout(function () { btn.removeAttribute('data-copied'); btn.innerHTML = ICON.copy + '<span>Copy</span>'; }, 1600);
        }, function () { btn.innerHTML = '<span>Select and copy</span>'; });
      });
      head.appendChild(btn);
      pre.parentNode.insertBefore(wrap, pre);
      wrap.appendChild(head); wrap.appendChild(pre);
      pre.setAttribute('tabindex', '0');
    });
  }

  /* ---------- Tables: scroll inside their own container ---------- */
  function wrapTables(root) {
    root.querySelectorAll('table').forEach(function (t) {
      if (t.parentElement.classList.contains('table-wrap')) return;
      var w = h('div', { class: 'table-wrap', tabindex: '0', role: 'region', 'aria-label': (t.getAttribute('aria-label') || (t.caption && t.caption.textContent) || 'Table') + ' (scrollable)' });
      t.parentNode.insertBefore(w, t); w.appendChild(t);
    });
  }

  /* ---------- Tabs: <div class="tabs" data-tabs><div data-tab="Label">...</div>...</div> ---------- */
  var tabCount = 0;
  function wireTabs(root) {
    root.querySelectorAll('[data-tabs]').forEach(function (box) {
      var panels = Array.prototype.slice.call(box.children).filter(function (c) { return c.hasAttribute('data-tab'); });
      var list = h('div', { class: 'tab-list', role: 'tablist', 'aria-label': box.getAttribute('data-tabs') || 'Examples' });
      var n = ++tabCount;
      var buttons = panels.map(function (p, i) {
        var id = 'tab-' + n + '-' + i;
        p.id = id + '-panel'; p.setAttribute('role', 'tabpanel'); p.setAttribute('aria-labelledby', id);
        var b = h('button', { type: 'button', role: 'tab', id: id, class: 'tab-btn', 'aria-controls': p.id, 'aria-selected': String(i === 0), tabindex: i === 0 ? '0' : '-1' }, esc(p.getAttribute('data-tab')));
        if (i) p.hidden = true;
        list.appendChild(b);
        return b;
      });
      function select(i, focus) {
        buttons.forEach(function (b, j) { b.setAttribute('aria-selected', String(j === i)); b.tabIndex = j === i ? 0 : -1; panels[j].hidden = j !== i; });
        if (focus) buttons[i].focus();
      }
      buttons.forEach(function (b, i) {
        b.addEventListener('click', function () { select(i); });
        b.addEventListener('keydown', function (e) {
          if (e.key === 'ArrowRight') { e.preventDefault(); select((i + 1) % buttons.length, true); }
          if (e.key === 'ArrowLeft') { e.preventDefault(); select((i - 1 + buttons.length) % buttons.length, true); }
          if (e.key === 'Home') { e.preventDefault(); select(0, true); }
          if (e.key === 'End') { e.preventDefault(); select(buttons.length - 1, true); }
        });
      });
      box.insertBefore(list, box.firstChild);
    });
  }

  /* ---------- Boot ---------- */
  function boot() {
    var current = document.body.getAttribute('data-page') || 'home';
    var page = PAGES.filter(function (p) { return p.id === current; })[0];
    if (document.getElementById('doc')) buildDocs(current); else buildLanding();
    if (page && !/KestroDB/.test(document.title)) document.title = page.title + ' · KestroDB';
    var content = document.getElementById('doc') || document.getElementById('landing');
    if (content) { wrapTables(content); enhanceCode(content); wireTabs(content); }
    wireTheme(); wireDrawer(); wireSearch(current);
    if (location.hash) { var t = document.getElementById(decodeURIComponent(location.hash.slice(1))); if (t) setTimeout(function () { t.scrollIntoView(); }, 0); }
  }
  if (document.readyState === 'loading') document.addEventListener('DOMContentLoaded', boot); else boot();
})();
