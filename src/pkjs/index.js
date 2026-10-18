var activeEntityId = localStorage.getItem('active_tv_entity') || "";

// Optional: pin a remote to its media_player if the automatic match picks the wrong one.
// e.g. "remote.quarto_tv_quarto_tv": "media_player.quarto_tv"
var MEDIA_OVERRIDES = {
};

var mediaMap = {};      // remote entity -> media_player entity (guessed from names)
var pollTimer = null;

// Polling: every 5 s while the remote is in use, once a minute when idle
var ACTIVE_MS = 30000;
var lastActivity = Date.now();

function getDeviceCommand(entityId, cmd) {
  var isAndroid = entityId.indexOf("android") !== -1 ||
                  entityId.indexOf("shield") !== -1 ||
                  entityId.indexOf("meo") !== -1 ||
                  entityId.indexOf("fire") !== -1;
  if (!isAndroid) {
    // Apple TV: power goes through send_command too
    var appleMap = { 'power_on': 'turn_on', 'power_off': 'turn_off' };
    return appleMap[cmd] || cmd;
  }
  var androidMap = {
    'up': 'DPAD_UP', 'down': 'DPAD_DOWN', 'left': 'DPAD_LEFT',
    'right': 'DPAD_RIGHT', 'select': 'DPAD_CENTER', 'menu': 'BACK',
    'volume_up': 'VOLUME_UP', 'volume_down': 'VOLUME_DOWN',
    'home': 'HOME', 'home_hold': 'HOME',
    'power_on': 'POWER', 'power_off': 'POWER'
  };
  return androidMap[cmd] || cmd;
}

function callService(service, payload) {
  var haUrl = localStorage.getItem('ha_url');
  var haToken = localStorage.getItem('ha_token');
  if (!haUrl || !haToken || !activeEntityId) return;
  if (haUrl.charAt(haUrl.length - 1) === '/') haUrl = haUrl.slice(0, -1);

  var xhr = new XMLHttpRequest();
  xhr.open('POST', haUrl + "/api/services/remote/" + service, true);
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
  callService('send_command', {
    "entity_id": activeEntityId,
    "command": [getDeviceCommand(activeEntityId, commandName)]
  });
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

function buildMediaMap(states, remotes) {
  var players = states.filter(function(s) { return s.entity_id.indexOf('media_player.') === 0; });
  mediaMap = {};
  remotes.forEach(function(r) {
    if (MEDIA_OVERRIDES[r.entity_id]) { mediaMap[r.entity_id] = MEDIA_OVERRIDES[r.entity_id]; return; }
    var rs = r.entity_id.slice(7);               // after "remote."
    var best = null, bestScore = -1;
    players.forEach(function(p) {
      var ps = p.entity_id.slice(13);            // after "media_player."
      var score = -1;
      if (ps === rs) score = 1000 + ps.length;
      else if (rs.indexOf(ps) === 0 || ps.indexOf(rs) === 0) score = Math.min(ps.length, rs.length);
      if (score > bestScore) { best = p; bestScore = score; }
    });
    if (best) mediaMap[r.entity_id] = best.entity_id;
  });
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

// Ask HA which media_player belongs to the same device as each remote.
// This is far more reliable than matching entity names.
function resolveMediaPlayers(done) {
  var haUrl = localStorage.getItem('ha_url');
  var haToken = localStorage.getItem('ha_token');
  if (!haUrl || !haToken) { done(); return; }
  if (haUrl.charAt(haUrl.length - 1) === '/') haUrl = haUrl.slice(0, -1);

  var tpl = "{% for r in states.remote %}{% set d = device_id(r.entity_id) %}{% if d %}" +
            "{% for e in device_entities(d) %}{% if e.startswith('media_player.') %}" +
            "{{ r.entity_id }}={{ e }};{% endif %}{% endfor %}{% endif %}{% endfor %}";

  var xhr = new XMLHttpRequest();
  xhr.open('POST', haUrl + '/api/template', true);
  xhr.setRequestHeader("Authorization", "Bearer " + haToken);
  xhr.setRequestHeader("Content-Type", "application/json");
  xhr.onload = function() {
    if (xhr.status === 200) {
      var seen = {};
      xhr.responseText.split(';').forEach(function(pair) {
        var kv = pair.split('=');
        if (kv.length !== 2) return;
        var r = kv[0].replace(/^\s+|\s+$/g, '');
        var m = kv[1].replace(/^\s+|\s+$/g, '');
        if (!r || !m || seen[r] || MEDIA_OVERRIDES[r]) return;
        seen[r] = true;
        mediaMap[r] = m;
      });
    } else {
      console.log("template lookup failed: " + xhr.status + " " + xhr.responseText);
    }
    console.log("media map: " + JSON.stringify(mediaMap));
    done();
  };
  xhr.onerror = function() { console.log("template lookup network error"); done(); };
  xhr.send(JSON.stringify({ template: tpl }));
}

function pollNowPlaying() {
  var haUrl = localStorage.getItem('ha_url');
  var haToken = localStorage.getItem('ha_token');
  if (!haUrl || !haToken || !activeEntityId) return;
  if (haUrl.charAt(haUrl.length - 1) === '/') haUrl = haUrl.slice(0, -1);

  var mp = MEDIA_OVERRIDES[activeEntityId] || mediaMap[activeEntityId];
  if (!mp) { enqueueNowPlaying({ '6': 'NO MP', '10': 0, '11': 0 }); return; }

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

    var title = off ? '' : (a.media_title || '');
    var sub = off ? '' : (a.media_artist || a.media_series_title || a.media_album_name || '');
    var app = off ? '' : (a.app_name || '');

    var dur = Math.round(a.media_duration || 0);
    var pos = Math.round(a.media_position || 0);
    if (st.state === 'playing' && a.media_position_updated_at && pos) {
      var t = Date.parse(String(a.media_position_updated_at).replace(/\.\d+/, ''));
      if (!isNaN(t)) pos += Math.max(0, Math.round((Date.now() - t) / 1000));
    }
    if (dur > 0 && pos > dur) pos = dur;
    if (off) { pos = 0; dur = 0; }

    var payload = { '6': tag, '10': pos, '11': dur };
    if (title) payload['7'] = String(title).substring(0, 60);
    if (sub) payload['8'] = String(sub).substring(0, 44);
    if (app) payload['9'] = String(app).substring(0, 28);
    enqueueNowPlaying(payload);
  };
  xhr.send();
}

/* ---------- TV list ---------- */

function fetchTVListFromHA() {
  var haUrl = localStorage.getItem('ha_url');
  var haToken = localStorage.getItem('ha_token');
  if (!haUrl || !haToken) return;
  if (haUrl.charAt(haUrl.length - 1) === '/') haUrl = haUrl.slice(0, -1);

  var xhr = new XMLHttpRequest();
  xhr.open('GET', haUrl + "/api/states", true);
  xhr.setRequestHeader("Authorization", "Bearer " + haToken);

  xhr.onload = function() {
    if (xhr.status === 200) {
      var states = JSON.parse(xhr.responseText);
      var remotes = states.filter(function(item) { return item.entity_id.indexOf('remote.') === 0; });

      // If the saved TV is missing or gone, default to the first remote so buttons always work
      var found = remotes.some(function(r) { return r.entity_id === activeEntityId; });
      if (!found && remotes.length) {
        activeEntityId = remotes[0].entity_id;
        localStorage.setItem('active_tv_entity', activeEntityId);
      }

      buildMediaMap(states, remotes);

      // A fresh list replaces anything still waiting to be sent
      sendQueue = [];

      enqueue({ '2': remotes.length });

      var activeFriendlyName = "";
      remotes.forEach(function(remote) {
        var name = remote.attributes.friendly_name || remote.entity_id;
        if (remote.entity_id === activeEntityId) activeFriendlyName = name;
        enqueue({ '3': name, '4': remote.entity_id });
      });

      // Tell the watch which TV is active so the title and highlight are right
      if (activeFriendlyName) enqueue({ '5': activeFriendlyName });

      resolveMediaPlayers(function() { pollNowPlaying(); });
    }
  };
  xhr.send();
}

Pebble.addEventListener('ready', function(e) {
  activeEntityId = localStorage.getItem('active_tv_entity') || "";
  lastActivity = Date.now();
  fetchTVListFromHA();
  schedulePoll();
});

Pebble.addEventListener('showConfiguration', function(e) {
  var currentUrl = localStorage.getItem('ha_url') || '';
  var currentToken = localStorage.getItem('ha_token') || '';
  var returnUrl = e.return_url || 'pebblejs://close#';
  Pebble.openURL('https://eddieatjollyroger.github.io/Pebble-HA-TV-Remote/?url=' +
    encodeURIComponent(currentUrl) + '&token=' + encodeURIComponent(currentToken) +
    '&return_to=' + encodeURIComponent(returnUrl));
});

Pebble.addEventListener('webviewclosed', function(e) {
  if (e.response) {
    try {
      var configData = JSON.parse(decodeURIComponent(e.response));
      if (configData.ha_url && configData.ha_token) {
        localStorage.setItem('ha_url', configData.ha_url);
        localStorage.setItem('ha_token', configData.ha_token);
        fetchTVListFromHA();
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
    fetchTVListFromHA();
  } else if (e.payload['5'] !== undefined) {
    // User picked a TV from the watch menu
    activeEntityId = e.payload['5'];
    localStorage.setItem('active_tv_entity', activeEntityId);
    pollNowPlaying();
  }
});
