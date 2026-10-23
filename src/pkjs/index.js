var activeEntityId = localStorage.getItem('active_tv_entity') || "";

// Optional: pin a remote to its media_player if the automatic match picks the wrong one.
// e.g. "remote.quarto_tv_quarto_tv": "media_player.quarto_tv"
var MEDIA_OVERRIDES = {
};

// Optional: what "power on" should do for a TV that can't be woken through its integration
// (for example an Android box in deep sleep). Key = the TV's entity in the watch's list.
// Value = any Home Assistant action to run instead, such as pressing a Wake-on-LAN button
// or running a script. Example:
//   "remote.mi_box": { domain: "button", service: "press", entity_id: "button.mi_box_wake" }
//   "remote.mi_box": { domain: "script", service: "turn_on", entity_id: "script.wake_mi_box" }
var WAKE_ACTIONS = {
};

// Remotes to leave out of the watch's list, by entity ID, for anything the automatic
// filter below doesn't catch. Example: ["remote.kitchen_speaker"]
var HIDDEN_TVS = [];

// HomePods show up as Apple TV remotes in Home Assistant, but their controls do nothing.
// They're hidden when the device's model, or its name, says HomePod.
function isHiddenTv(id, name, model) {
  if (HIDDEN_TVS.indexOf(id) !== -1) return true;
  return /homepod/i.test(String(model || '')) || /homepod/i.test(String(name || ''));
}

var tvTypes = {};       // entity -> 'apple' | 'android' | 'vidaa' | 'generic'
var mediaMap = {};      // remote entity -> media_player entity (guessed from names)
var pollTimer = null;

// Theme chosen on the settings page: 'dark' (default) or 'light'. Sent to the watch as 0 / 1.
function themeValue() {
  return localStorage.getItem('theme') === 'light' ? 1 : 0;
}

// Polling: every 5 s while the remote is in use, once a minute when idle
var ACTIVE_MS = 30000;
var lastActivity = Date.now();

// VIDAA TVs are driven through Home Assistant button entities, named
// button.<device>_<key>, exactly like your universal remote script. A TV counts as VIDAA
// when "vidaa" appears in its entity name.
// The kind of TV, found from the Home Assistant integration that owns it (see fetchRemotes).
// Until that's known, fall back to guessing from the entity name.
function tvType(entityId) {
  if (tvTypes[entityId]) return tvTypes[entityId];
  var name = entityId.split('.').pop();
  if (name.indexOf('vidaa') !== -1) return 'vidaa';
  if (/android|shield|fire/.test(name)) return 'android';
  return 'generic';
}

function typeFromIntegration(integration) {
  if (integration === 'apple_tv') return 'apple';
  if (integration === 'androidtv_remote' || integration === 'androidtv') return 'android';
  if (integration === 'vidaa_tv') return 'vidaa';
  return 'generic';       // any other TV: standard Home Assistant commands
}

// Watch command -> the key part of the button entity name.
// up/down/left/right/ok/stop/home come from your script (Back = stop, like there).
// volume_* and power are my best guess: check the button entities your VIDAA TV
// actually has in Home Assistant and edit these names if they differ.
var VIDAA_KEYS = {
  'up': 'up', 'down': 'down', 'left': 'left', 'right': 'right',
  'select': 'ok',
  'menu': 'stop',
  'home': 'home', 'home_hold': 'home',
  'volume_up': 'volume_up', 'volume_down': 'volume_down',
  'power_on': 'power', 'power_off': 'power'
};

// Command names for remote.send_command, per kind of TV
var ANDROID_KEYS = {
  'up': 'DPAD_UP', 'down': 'DPAD_DOWN', 'left': 'DPAD_LEFT', 'right': 'DPAD_RIGHT',
  'select': 'DPAD_CENTER', 'menu': 'BACK',
  'volume_up': 'VOLUME_UP', 'volume_down': 'VOLUME_DOWN',
  'home': 'HOME', 'home_hold': 'HOME',
  'power_on': 'POWER', 'power_off': 'POWER'
};
var APPLE_KEYS = { 'power_on': 'turn_on', 'power_off': 'turn_off' };   // the rest are sent as-is
// Plain names that Roku-style integrations accept. Other TVs may need a different set.
var GENERIC_KEYS = { 'menu': 'back', 'home_hold': 'home' };

function remoteCommand(type, cmd) {
  if (type === 'android') return ANDROID_KEYS[cmd] || cmd;
  if (type === 'apple') return APPLE_KEYS[cmd] || cmd;
  return GENERIC_KEYS[cmd] || cmd;
}

function callService(domain, service, payload) {
  var haUrl = localStorage.getItem('ha_url');
  var haToken = localStorage.getItem('ha_token');
  if (!haUrl || !haToken || !activeEntityId) return;
  if (haUrl.charAt(haUrl.length - 1) === '/') haUrl = haUrl.slice(0, -1);

  var xhr = new XMLHttpRequest();
  xhr.open('POST', haUrl + "/api/services/" + domain + "/" + service, true);
  xhr.setRequestHeader("Authorization", "Bearer " + haToken);
  xhr.setRequestHeader("Content-Type", "application/json");
  xhr.onreadystatechange = function () {
    if (xhr.readyState === 4 && (xhr.status < 200 || xhr.status >= 300)) {
      console.log("HA error " + xhr.status + ": " + xhr.responseText);
    }
  };
  xhr.send(JSON.stringify(payload));
}

function sendCommandToHA(commandName) {
  var mp = mediaMap[activeEntityId];   // the TV's media_player, if we found one

  // Reload the TV's integration in Home Assistant (for when a connection goes stale)
  if (commandName === 'reload') {
    callService('homeassistant', 'reload_config_entry', { "entity_id": mp || activeEntityId });
    setTimeout(pollNowPlaying, 6000);
    return;
  }

  // A custom wake action for this TV replaces the normal "power on"
  if (commandName === 'power_on' && WAKE_ACTIONS[activeEntityId]) {
    var w = WAKE_ACTIONS[activeEntityId];
    callService(w.domain, w.service, { "entity_id": w.entity_id });
    setTimeout(pollNowPlaying, 8000);
    return;
  }

  var type = tvType(activeEntityId);

  if (type === 'vidaa') {
    // Power and volume go through the media player, the same calls HomeKit makes.
    // Power on is a Wake-on-LAN packet there, so it works even when the TV's control
    // connection is down. Everything else presses the TV's button entities.
    var mpService = null;
    if (commandName === 'power_on') mpService = 'turn_on';
    else if (commandName === 'power_off') mpService = 'turn_off';
    else if (commandName === 'volume_up') mpService = 'volume_up';
    else if (commandName === 'volume_down') mpService = 'volume_down';

    if (mpService && mp) {
      callService('media_player', mpService, { "entity_id": mp });
    } else {
      var key = VIDAA_KEYS[commandName];
      if (key) {
        var base = activeEntityId.split('.').pop();
        callService('button', 'press', { "entity_id": "button." + base + "_" + key });
      }
    }
  } else if (type === 'generic') {
    // A TV we don't recognize: use the standard Home Assistant actions where there is one
    // (power and volume on its media player), and send the key names for the rest.
    var generic = { 'power_on': 'turn_on', 'power_off': 'turn_off', 'volume_up': 'volume_up', 'volume_down': 'volume_down' }[commandName];
    if (generic && mp) {
      callService('media_player', generic, { "entity_id": mp });
    } else if (generic && (commandName === 'power_on' || commandName === 'power_off')) {
      callService('remote', generic, { "entity_id": activeEntityId });
    } else {
      callService('remote', 'send_command', {
        "entity_id": activeEntityId,
        "command": [remoteCommand('generic', commandName)]
      });
    }
  } else {
    // Apple TV and Android TV: remote.send_command with that kind's key names
    callService('remote', 'send_command', {
      "entity_id": activeEntityId,
      "command": [remoteCommand(type, commandName)]
    });
  }
  // Refresh now-playing shortly after a command (pause, power, etc.)
  setTimeout(pollNowPlaying, 1500);
}

/* ---------- Messages to the watch: one at a time, with ACK and retry ---------- */

var sendQueue = [];
var inflight = null;

function pumpQueue() {
  if (inflight || sendQueue.length === 0) return;
  inflight = sendQueue.shift();
  var item = inflight;
  Pebble.sendAppMessage(item.payload, function() {
    inflight = null;
    pumpQueue();
  }, function() {
    item.tries++;
    if (item.tries < item.maxTries) sendQueue.unshift(item);
    inflight = null;
    setTimeout(pumpQueue, 250);
  });
}

function enqueue(payload, maxTries) {
  sendQueue.push({ payload: payload, tries: 0, maxTries: maxTries || 5 });
  pumpQueue();
}

// Now-playing updates replace any older one still waiting, and aren't retried
function enqueueNowPlaying(payload) {
  sendQueue = sendQueue.filter(function(i) { return i.payload['6'] === undefined; });
  enqueue(payload, 1);
}

/* ---------- Now playing ---------- */

function stateTag(s) {
  switch (s) {
    case 'playing': return 'PLAY';
    case 'paused': return 'PAUSE';
    case 'buffering': return 'BUFFER';
    case 'idle': return 'IDLE';
    case 'standby':
    case 'off': return 'OFF';
    case 'unavailable': return 'OFFLINE';
    default: return s ? String(s).toUpperCase().substring(0, 7) : '--';
  }
}

// Fallback when the template lookup isn't available: guess the media_player from names
function buildMediaMap(states, remotes) {
  var players = states.filter(function(st) { return st.entity_id.indexOf('media_player.') === 0; });
  var map = {};
  remotes.forEach(function(r) {
    var rs = r.id.slice(7);                      // after "remote."
    var best = null, bestScore = -1;
    players.forEach(function(p) {
      var ps = p.entity_id.slice(13);            // after "media_player."
      var score = -1;
      if (ps === rs) score = 1000 + ps.length;
      else if (rs.indexOf(ps) === 0 || ps.indexOf(rs) === 0) score = Math.min(ps.length, rs.length);
      if (score > bestScore) { best = p; bestScore = score; }
    });
    if (best) map[r.id] = best.entity_id;
  });
  return map;
}

function isActive() { return Date.now() - lastActivity < ACTIVE_MS; }

function schedulePoll() {
  if (pollTimer) clearTimeout(pollTimer);
  pollTimer = setTimeout(function() {
    pollNowPlaying();
    schedulePoll();
  }, isActive() ? 5000 : 60000);
}

function noteActivity() {
  var wasIdle = !isActive();
  lastActivity = Date.now();
  if (wasIdle) { pollNowPlaying(); schedulePoll(); }
}

function pollNowPlaying() {
  var haUrl = localStorage.getItem('ha_url');
  var haToken = localStorage.getItem('ha_token');
  if (!haUrl || !haToken || !activeEntityId) return;
  if (haUrl.charAt(haUrl.length - 1) === '/') haUrl = haUrl.slice(0, -1);

  var mp = MEDIA_OVERRIDES[activeEntityId] || mediaMap[activeEntityId];
  if (!mp) {
    if (tvCache) enqueueNowPlaying({ '6': 'NO MP', '10': 0, '11': 0 });   // lookup done, none found
    return;
  }

  var xhr = new XMLHttpRequest();
  xhr.open('GET', haUrl + "/api/states/" + mp, true);
  xhr.setRequestHeader("Authorization", "Bearer " + haToken);
  xhr.onload = function() {
    if (xhr.status === 404) { enqueueNowPlaying({ '6': '404', '10': 0, '11': 0 }); return; }
    if (xhr.status !== 200) { enqueueNowPlaying({ '6': 'E' + xhr.status, '10': 0, '11': 0 }); return; }
    var st = JSON.parse(xhr.responseText);
    var a = st.attributes || {};
    var tag = stateTag(st.state);
    var off = (tag === 'OFF' || tag === 'OFFLINE');
    console.log('media_player ' + mp + ' state=' + st.state + ' attrs=' + Object.keys(a).join(','));

    // Show whatever HA reports, even when its state says "off": some Apple TVs
    // report the wrong power state while still exposing what's playing.
    var title = a.media_title || '';
    var sub = a.media_artist || a.media_series_title || a.media_album_name || '';
    var app = a.app_name || '';
    // Android TV and VIDAA players don't report titles or progress. Show the app (or the
    // input) as the main line instead, so there's still something useful on screen.
    if (!title) {
      if (app) { title = app; app = ''; }
      else if (a.source) { title = a.source; }
    }

    var dur = Math.round(a.media_duration || 0);
    var pos = Math.round(a.media_position || 0);
    if (st.state === 'playing' && a.media_position_updated_at && pos) {
      var t = Date.parse(String(a.media_position_updated_at).replace(/\.\d+/, ''));
      if (!isNaN(t)) pos += Math.max(0, Math.round((Date.now() - t) / 1000));
    }
    if (dur > 0 && pos > dur) pos = dur;
    if (off && !title) { pos = 0; dur = 0; }

    var payload = { '6': tag, '10': pos, '11': dur };
    if (title) payload['7'] = String(title).substring(0, 60);
    if (sub) payload['8'] = String(sub).substring(0, 44);
    if (app) payload['9'] = String(app).substring(0, 28);
    enqueueNowPlaying(payload);
  };
  xhr.send();
}

/* ---------- TV list ---------- */

// The list and the remote -> media_player map are cached on the phone, so launching
// the app never waits on Home Assistant before showing now-playing.
var tvCache = null;   // [{ id: 'remote.x', name: 'X' }, ...]

function loadCaches() {
  try { tvCache = JSON.parse(localStorage.getItem('tv_cache') || 'null'); } catch (e) { tvCache = null; }
  try { mediaMap = JSON.parse(localStorage.getItem('media_map') || '{}') || {}; } catch (e) { mediaMap = {}; }
  try { tvTypes = JSON.parse(localStorage.getItem('tv_types') || '{}') || {}; } catch (e) { tvTypes = {}; }
}

function saveCaches() {
  try {
    localStorage.setItem('tv_cache', JSON.stringify(tvCache));
    localStorage.setItem('media_map', JSON.stringify(mediaMap));
    localStorage.setItem('tv_types', JSON.stringify(tvTypes));
  } catch (e) { }
}

function trim(str) { return String(str || '').replace(/^\s+|\s+$/g, ''); }

// Older/odd setups: download all states (large) and pick out the remotes
function fetchRemotesFromStates(done) {
  var haUrl = localStorage.getItem('ha_url');
  var haToken = localStorage.getItem('ha_token');
  if (!haUrl || !haToken) { done(null); return; }
  if (haUrl.charAt(haUrl.length - 1) === '/') haUrl = haUrl.slice(0, -1);

  var xhr = new XMLHttpRequest();
  xhr.open('GET', haUrl + "/api/states", true);
  xhr.setRequestHeader("Authorization", "Bearer " + haToken);
  xhr.onload = function() {
    if (xhr.status !== 200) { done(null); return; }
    var states = JSON.parse(xhr.responseText);
    var remotes = states.filter(function(it) { return it.entity_id.indexOf('remote.') === 0; })
      .map(function(it) { return { id: it.entity_id, name: it.attributes.friendly_name || it.entity_id }; });
    // VIDAA TVs: found through their "..._ok" button, unless they also have a remote entity
    states.forEach(function(it) {
      var id = it.entity_id;
      if (id.indexOf('button.') !== 0 || id.indexOf('vidaa') === -1 || !/_ok$/.test(id)) return;
      var base = id.slice(7).replace(/_ok$/, '');
      // The integration's own remote entity (e.g. remote.<base>_remote) is a dead duplicate
      remotes = remotes.filter(function(r) { return r.id.slice(7).indexOf(base) !== 0; });
      remotes.push({ id: 'button.' + base, name: String(it.attributes.friendly_name || base).replace(/\s+ok$/i, '') });
    });
    remotes = remotes.filter(function(r) { return !isHiddenTv(r.id, r.name, ''); });
    done(remotes, buildMediaMap(states, remotes));
  };
  xhr.onerror = function() { done(null); };
  xhr.send();
}

// One small template call returns every remote, its name, and the media_player on the
// same device. Much lighter than downloading every entity in Home Assistant.
function fetchRemotes(done) {
  var haUrl = localStorage.getItem('ha_url');
  var haToken = localStorage.getItem('ha_token');
  if (!haUrl || !haToken) { done(null); return; }
  if (haUrl.charAt(haUrl.length - 1) === '/') haUrl = haUrl.slice(0, -1);

  // The media_player on the same device as an entity (space separated if several)
  function mpFor(ent, v) {
    return "{% set " + v + " = device_id(" + ent + ") %}{% if " + v + " %}" +
           "{% for e in device_entities(" + v + ") %}{% if e.startswith('media_player.') %}{{ e }} {% endif %}" +
           "{% endfor %}{% endif %}";
  }
  // Which known integration owns an entity (empty if none of them)
  function typeTpl(ent) {
    return "{% for i in ['apple_tv','androidtv_remote','androidtv','vidaa_tv'] %}" +
           "{% if " + ent + " in integration_entities(i) %}{{ i }}{% endif %}{% endfor %}";
  }
  var tpl =
    "{% for r in states.remote %}{{ r.entity_id }}\t{{ r.name }}\t" + mpFor("r.entity_id", "d") + "\t{{ d }}\t" + typeTpl("r.entity_id") + "\t{{ device_attr(d, 'model') if d else '' }}\n{% endfor %}" +
    "{% for b in states.button if 'vidaa' in b.entity_id and b.entity_id.endswith('_ok') %}" +
    "{{ b.entity_id }}\t{% set bd = device_id(b.entity_id) %}" +
    "{{ (device_attr(bd, 'name_by_user') or device_attr(bd, 'name')) if bd else b.name }}\t" +
    mpFor("b.entity_id", "bd2") + "\t{{ bd2 }}\n{% endfor %}";

  var xhr = new XMLHttpRequest();
  xhr.open('POST', haUrl + '/api/template', true);
  xhr.setRequestHeader("Authorization", "Bearer " + haToken);
  xhr.setRequestHeader("Content-Type", "application/json");
  xhr.onload = function() {
    if (xhr.status === 200) {
      var remotes = [], map = {}, buttons = [], devOf = {}, types = {}, models = {};
      xhr.responseText.split('\n').forEach(function(line) {
        var p = line.split('\t');
        var id = trim(p[0]);
        var name = trim(p[1]);
        var mp = trim(p[2]).split(' ')[0];
        var dev = trim(p[3]);
        if (dev === 'None') dev = '';
        if (id.indexOf('remote.') === 0) {
          remotes.push({ id: id, name: name || id });
          if (mp) map[id] = mp;
          if (dev) devOf[id] = dev;
          types[id] = typeFromIntegration(trim(p[4]));
          var model = trim(p[5]);
          if (model !== 'None') models[id] = model;
        } else if (id.indexOf('button.') === 0) {
          var base = id.slice(7).replace(/_ok$/, '');
          buttons.push({ id: 'button.' + base, name: name || base, mp: mp, dev: dev });
        }
      });
      // A VIDAA TV is listed through its buttons, so drop the integration's own "remote"
      // entity for that same device (it would show up as a second, dead entry)
      var buttonDevs = {};
      buttons.forEach(function(bt) { if (bt.dev) buttonDevs[bt.dev] = true; });
      remotes = remotes.filter(function(r) { return !(devOf[r.id] && buttonDevs[devOf[r.id]]); });
      // Leave out HomePods and anything on the hidden list
      remotes = remotes.filter(function(r) { return !isHiddenTv(r.id, r.name, models[r.id]); });
      // A VIDAA TV that also has a remote entity is already listed once; don't add it twice
      buttons.forEach(function(bt) {
        var dup = remotes.some(function(r) { return r.id.slice(7) === bt.id.slice(7); });
        if (!dup) {
          remotes.push({ id: bt.id, name: bt.name });
          if (bt.mp) map[bt.id] = bt.mp;
          types[bt.id] = 'vidaa';
        }
      });
      if (remotes.length) { done(remotes, map, types); return; }
    }
    console.log("template lookup unusable (" + xhr.status + "), falling back to /api/states");
    fetchRemotesFromStates(done);
  };
  xhr.onerror = function() { console.log("template lookup network error"); done(null); };
  xhr.send(JSON.stringify({ template: tpl }));
}

// Store a fresh list + map. Returns true if anything changed.
function applyRemotes(remotes, map, types) {
  var newMap = {};
  Object.keys(map || {}).forEach(function(k) { newMap[k] = map[k]; });
  remotes.forEach(function(r) { if (MEDIA_OVERRIDES[r.id]) newMap[r.id] = MEDIA_OVERRIDES[r.id]; });

  var newTypes = types || {};
  var changed = JSON.stringify(remotes) !== JSON.stringify(tvCache) ||
                JSON.stringify(newMap) !== JSON.stringify(mediaMap) ||
                JSON.stringify(newTypes) !== JSON.stringify(tvTypes);
  tvCache = remotes;
  mediaMap = newMap;
  tvTypes = newTypes;
  saveCaches();

  // If the saved TV is missing or gone, default to the first remote so buttons always work
  var found = remotes.some(function(r) { return r.id === activeEntityId; });
  if (!found && remotes.length) {
    activeEntityId = remotes[0].id;
    localStorage.setItem('active_tv_entity', activeEntityId);
    changed = true;
  }
  console.log("media map: " + JSON.stringify(mediaMap) + " types: " + JSON.stringify(tvTypes));
  return changed;
}

function sendListToWatch() {
  if (!tvCache) return;
  // Replace any older list messages still waiting; leave now-playing alone
  sendQueue = sendQueue.filter(function(i) {
    var p = i.payload;
    return p['2'] === undefined && p['3'] === undefined && p['5'] === undefined;
  });

  enqueue({ '2': tvCache.length });
  var activeName = "";
  tvCache.forEach(function(r) {
    if (r.id === activeEntityId) activeName = r.name;
    enqueue({ '3': r.name, '4': r.id });
  });
  // Tell the watch which TV is active so the title and highlight are right
  if (activeName) enqueue({ '5': activeName });
}

// Refresh from Home Assistant. Resends the list to the watch if asked to, or if it changed.
function refreshRemotes(sendToWatch) {
  fetchRemotes(function(remotes, map, types) {
    if (!remotes) return;
    var changed = applyRemotes(remotes, map || {}, types);
    if (sendToWatch || changed) sendListToWatch();
    if (changed) pollNowPlaying();
  });
}

Pebble.addEventListener('ready', function(e) {
  activeEntityId = localStorage.getItem('active_tv_entity') || "";
  lastActivity = Date.now();
  loadCaches();

  enqueue({ '12': themeValue() });   // keep the watch's theme in sync
  pollNowPlaying();                  // uses the cached media map: no waiting on HA
  schedulePoll();

  // Refresh the list and map in the background. First run has no cache, so do it now.
  setTimeout(function() { refreshRemotes(false); }, tvCache ? 3000 : 0);
});

Pebble.addEventListener('showConfiguration', function(e) {
  var currentUrl = localStorage.getItem('ha_url') || '';
  var currentToken = localStorage.getItem('ha_token') || '';
  var returnUrl = e.return_url || 'pebblejs://close#';
  Pebble.openURL('https://eddieatjollyroger.github.io/Pebble-HA-TV-Remote/?url=' +
    encodeURIComponent(currentUrl) + '&token=' + encodeURIComponent(currentToken) +
    '&theme=' + (themeValue() ? 'light' : 'dark') +
    '&return_to=' + encodeURIComponent(returnUrl));
});

Pebble.addEventListener('webviewclosed', function(e) {
  if (e.response) {
    try {
      var configData = JSON.parse(decodeURIComponent(e.response));
      if (configData.theme === 'light' || configData.theme === 'dark') {
        localStorage.setItem('theme', configData.theme);
      }
      if (configData.ha_url && configData.ha_token) {
        localStorage.setItem('ha_url', configData.ha_url);
        localStorage.setItem('ha_token', configData.ha_token);
        enqueue({ '12': themeValue() });
        refreshRemotes(true);
      } else {
        enqueue({ '12': themeValue() });
      }
    } catch(err) {
      console.log("Config parse error: " + err);
    }
  }
});

Pebble.addEventListener('appmessage', function(e) {
  if (!e.payload) return;
  noteActivity();
  if (e.payload['0'] !== undefined) {
    sendCommandToHA(e.payload['0']);
  } else if (e.payload['1'] !== undefined) {
    // The watch opened its TV list: answer from the cache right away, then refresh quietly
    if (tvCache) { sendListToWatch(); refreshRemotes(false); }
    else refreshRemotes(true);
  } else if (e.payload['5'] !== undefined) {
    // User picked a TV from the watch menu
    activeEntityId = e.payload['5'];
    localStorage.setItem('active_tv_entity', activeEntityId);
    pollNowPlaying();
  }
});
