'use strict';

// SMCA LAN config page.
//
// The rule this file follows: it renders what the console tells it and nothing
// else. Every change is a POST to /api/draft/*, and only "Apply" writes the config
// — the pending list in the bottom bar is the console's view of exactly the same
// draft, so the two screens cannot disagree.
//
// Accounts are listed, never touched: the cards are read-only, the only action is
// "add one". Editing and deleting a connection belong to the console — an address
// identifies a server, and removing a profile can clear the active one, i.e. rebuild
// the shell that is hosting this page. A row this page added can be taken back
// ("Undo") until Apply writes it.

const $ = (id) => document.getElementById(id);
let strings = {};
let lastState = null;
let poll = null;
let connSig = '';

const t = (k) => strings[k] || k;

function applyStrings() {
  document.querySelectorAll('[data-k]').forEach((el) => {
    el.textContent = t(el.getAttribute('data-k'));
  });
}

async function jfetch(path, opts) {
  const res = await fetch(path, Object.assign({
    headers: { 'Content-Type': 'application/json' },
    credentials: 'same-origin',
    cache: 'no-store',
  }, opts || {}));
  let body = {};
  try { body = await res.json(); } catch (e) { /* 204 or a proxy error page */ }
  return { res, body };
}

// ---------------------------------------------------------------- screens

function showPin(message) {
  $('editor').classList.add('hidden');
  $('bar').classList.add('hidden');
  $('pinScreen').classList.remove('hidden');
  $('pinErr').textContent = message || '';
}

function showEditor() {
  $('pinScreen').classList.add('hidden');
  $('editor').classList.remove('hidden');
  $('bar').classList.remove('hidden');
}

function renderPin(s) {
  showPin('');
  const usable = s.phase === 'armed';
  $('pin').classList.toggle('hidden', !usable);
  $('pinGo').classList.toggle('hidden', !usable);
  if (!usable) $('pinErr').textContent = s.phase === 'paused' ? t('pin_paused') : t('pin_idle');
}

async function load(full) {
  const { res, body } = await jfetch('/api/state');
  if (!res.ok) { showPin(t('failed')); return; }
  lastState = body;
  strings = body.strings || strings;
  applyStrings();

  if (body.need === 'pin') { renderPin(body); return; }

  showEditor();
  if (full) {
    // Only on the first load and after a claim: the fields are the user's while
    // the poll runs underneath them.
    $('ua').value = (body.current && body.current.ua) || '';
    $('proxy').value = (body.current && body.current.proxy) || '';
  }
  renderWho(body);
  renderConnections(body);
  renderPending(body);
}

function renderWho(s) {
  const active = (s.connections || []).find((c) => c.active);
  $('who').textContent = active ? `${t('conn_active')}: ${active.name || active.url}` : '';
}

function renderPending(s) {
  const changes = (s.draft && s.draft.changes) || [];
  const box = $('pending');
  box.textContent = '';
  if (!changes.length) {
    box.textContent = t('pending_none');
  } else {
    const head = document.createElement('div');
    head.textContent = t('pending_title');
    box.appendChild(head);
    for (const c of changes) {
      const line = document.createElement('div');
      line.className = 'pinItem';
      const mark = c.kind === 'add' ? '+ ' : c.kind === 'remove' ? '- ' : '· ';
      const label = c.kind === 'ua' ? t('ua_label')
                  : c.kind === 'proxy' ? t('proxy_label')
                  : (c.name || '?');
      line.textContent = mark + label;
      box.appendChild(line);
    }
  }
  $('apply').disabled = !(s.draft && s.draft.ready && changes.length);
  $('discard').disabled = !changes.length;
}

// ---------------------------------------------------------------- accounts

function chip(text, cls) {
  const el = document.createElement('span');
  el.className = 'chip' + (cls ? ' ' + cls : '');
  el.textContent = text;
  return el;
}

function kvRow(dl, label, value) {
  if (!value) return;
  const row = document.createElement('div');
  const dt = document.createElement('dt');
  dt.textContent = label;
  const dd = document.createElement('dd');
  dd.textContent = value;
  row.appendChild(dt);
  row.appendChild(dd);
  dl.appendChild(row);
}

/// One card per account. Accounts are read-only here: switching, editing and
/// removing one is the console's job. The only button a card can carry belongs to a
/// row that is still pending — an account this page just logged in and Apply has not
/// written yet, which can be taken back.
function accountCard(c, pending) {
  const card = document.createElement('div');
  card.className = 'card acct';
  if (c.active) card.setAttribute('data-active', '1');
  if (pending) card.setAttribute('data-pending', '1');

  const head = document.createElement('div');
  head.className = 'acct-head';
  const name = document.createElement('div');
  name.className = 'acct-name';
  name.textContent = c.name || c.url || '?';
  head.appendChild(name);

  const chips = document.createElement('div');
  chips.className = 'chips';
  chips.appendChild(chip(c.type === 'emby' ? 'Emby' : 'Jellyfin'));
  if (c.active) chips.appendChild(chip(t('conn_active'), 'on'));
  if (pending) chips.appendChild(chip(t('conn_pending_add'), 'add'));
  head.appendChild(chips);
  card.appendChild(head);

  const kv = document.createElement('dl');
  kv.className = 'kv';
  kvRow(kv, t('conn_addr'), c.url);
  kvRow(kv, t('conn_lan_short'), c.local_url);
  kvRow(kv, t('conn_user'), c.user_name);
  card.appendChild(kv);

  if (pending) {
    const foot = document.createElement('div');
    foot.className = 'acct-foot';
    const undo = document.createElement('button');
    undo.textContent = t('undo');
    undo.onclick = async () => {
      await jfetch('/api/draft/forget', { method: 'POST', body: JSON.stringify({ id: c.id }) });
      load(false);
    };
    foot.appendChild(undo);
    card.appendChild(foot);
  }
  return card;
}

function renderConnections(s) {
  const applied = s.connections || [];
  const rows = (s.draft && s.draft.rows) || [];

  const view = [];
  const seen = {};
  for (const c of applied) {
    seen[c.id] = true;
    view.push({ c: c, pending: false });
  }
  for (const r of rows) {
    // A row that is already applied (the same account logged in twice) needs no
    // second card; the console list is the truth once Apply has run.
    if (seen[r.id]) continue;
    view.push({ c: r, pending: true });
  }

  // The poll runs every 1.5 s; rebuilding the DOM each time would throw away the
  // button under a finger. Rebuild only when the picture actually changed.
  const sig = JSON.stringify(view);
  if (sig === connSig) return;
  connSig = sig;

  const box = $('conns');
  box.textContent = '';
  for (const v of view) box.appendChild(accountCard(v.c, v.pending));
}

// ---------------------------------------------------------------- actions

$('pinGo').onclick = async () => {
  const pin = $('pin').value.replace(/\D/g, '').slice(0, 6);
  if (pin.length !== 6) return;
  const { res, body } = await jfetch('/api/session', { method: 'POST', body: JSON.stringify({ pin }) });
  if (res.ok) { $('pin').value = ''; load(true); return; }
  if (res.status === 401) {
    $('pinErr').textContent = `${t('pin_wrong')} (${body.left})`;
  } else if (res.status === 423) {
    $('pinErr').textContent = t('pin_paused');
  } else if (res.status === 409) {
    $('pinErr').textContent = body.ip ? `${t('claimed_other')} ${body.ip}` : t('claimed_other');
  } else {
    $('pinErr').textContent = t('failed');
  }
};

$('pin').addEventListener('keydown', (e) => { if (e.key === 'Enter') $('pinGo').click(); });

document.querySelectorAll('[data-save]').forEach((btn) => {
  btn.onclick = async () => {
    const which = btn.getAttribute('data-save');
    const value = which === 'ua' ? $('ua').value : $('proxy').value;
    const { res } = await jfetch('/api/draft/' + which, { method: 'POST', body: JSON.stringify({ value }) });
    btn.textContent = res.ok ? t('saved') : t('failed');
    setTimeout(() => { btn.textContent = t('conn_save'); }, 1200);
    load(false);
  };
});

$('addGo').onclick = async () => {
  $('addErr').textContent = '';
  const payload = {
    type: $('newType').value,
    url: $('newUrl').value,
    local_url: $('newLan').value,
    name: $('newName').value,
    user: $('newUser').value,
    password: $('newPass').value,
  };
  $('addGo').disabled = true;
  const { res, body } = await jfetch('/api/draft/login', { method: 'POST', body: JSON.stringify(payload) });
  $('addGo').disabled = false;
  if (!res.ok) {
    $('addErr').textContent = body.error || t('failed');
    return;
  }
  ['newUrl', 'newLan', 'newName', 'newUser', 'newPass'].forEach((id) => { $(id).value = ''; });
  load(false);
};

$('apply').onclick = async () => {
  const { res } = await jfetch('/api/apply', { method: 'POST', body: '{}' });
  if (res.ok) $('pending').textContent = t('saved');
  load(false);
};

$('discard').onclick = async () => {
  await jfetch('/api/draft/discard', { method: 'POST', body: '{}' });
  load(false);
};

// ---------------------------------------------------------------- boot

// One poll for both screens: it is how a PIN that appeared on the console shows up
// here, and how a session that ended (idle, or X on the console) is noticed.
function startPoll() {
  if (poll) return;
  poll = setInterval(() => { load(false).catch(() => {}); }, 1500);
}

load(true).then(startPoll).catch(() => showPin(t('failed')));
