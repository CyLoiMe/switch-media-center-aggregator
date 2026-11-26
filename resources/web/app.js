'use strict';

// SMCA LAN config page.
//
// The rule this file follows: it renders what the console tells it and nothing
// else. Every change is a POST to /api/draft/*, and only "Apply" writes the config
// — the pending list in the bottom bar is the console's view of exactly the same
// draft, so the two screens cannot disagree.

const $ = (id) => document.getElementById(id);
let strings = {};
let lastState = null;
let poll = null;

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
    $('ua').value = (body.current && body.current.ua) || '';
    $('proxy').value = (body.current && body.current.proxy) || '';
    renderConnections(body);
  }
  renderWho(body);
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
      const mark = c.kind === 'add' ? '+ ' : c.kind === 'edit' ? '~ ' : c.kind === 'remove' ? '- ' : '· ';
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

function renderConnections(s) {
  const box = $('conns');
  box.textContent = '';

  for (const c of s.connections || []) {
    const row = document.createElement('div');
    row.className = 'conn';

    if (c.active) {
      // Editing the connection being browsed would need resetBackend() and a shell
      // rebuild — and the shell is the page hosting this portal. Say so instead of
      // offering a button that cannot work.
      const name = document.createElement('div');
      name.textContent = `${c.name || c.url}  —  ${t('conn_active')}`;
      const why = document.createElement('div');
      why.className = 'muted';
      why.textContent = t('conn_active_hint');
      row.appendChild(name);
      row.appendChild(why);
      box.appendChild(row);
      continue;
    }

    const nameInput = document.createElement('input');
    nameInput.value = c.name || '';
    nameInput.placeholder = t('conn_name');

    const laneInput = document.createElement('input');
    laneInput.value = c.local_url || '';
    laneInput.placeholder = t('conn_lan');
    laneInput.autocapitalize = 'off';
    laneInput.spellcheck = false;

    const typeSel = document.createElement('select');
    for (const ty of ['jellyfin', 'emby']) {
      const o = document.createElement('option');
      o.value = ty;
      o.textContent = ty === 'emby' ? 'Emby' : 'Jellyfin';
      if ((c.type || 'jellyfin') === ty) o.selected = true;
      typeSel.appendChild(o);
    }

    const addr = document.createElement('div');
    addr.className = 'muted';
    addr.textContent = `${c.url || ''}  (${t('conn_addr_locked')})`;

    const save = document.createElement('button');
    save.textContent = t('conn_save');
    save.onclick = async () => {
      await jfetch('/api/draft/connection', {
        method: 'POST',
        body: JSON.stringify({ id: c.id, name: nameInput.value, local_url: laneInput.value, type: typeSel.value }),
      });
      load(false);
    };

    const del = document.createElement('button');
    del.className = 'danger';
    del.textContent = t('conn_delete');
    del.onclick = async () => {
      if (!confirm(t('delete_confirm'))) return;
      await jfetch('/api/draft/connection/' + encodeURIComponent(c.id), { method: 'DELETE' });
      load(false);
    };

    const act = document.createElement('div');
    act.className = 'row';
    act.appendChild(save);
    act.appendChild(del);

    row.appendChild(nameInput);
    row.appendChild(laneInput);
    row.appendChild(typeSel);
    row.appendChild(addr);
    row.appendChild(act);
    box.appendChild(row);
  }
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
