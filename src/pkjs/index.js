var activeEntityId = localStorage.getItem('active_tv_entity') || "";

function getDeviceCommand(entityId, cmd) {
  var isAndroid = entityId.indexOf("android") !== -1 ||
                  entityId.indexOf("shield") !== -1 ||
                  entityId.indexOf("fire") !== -1;
  if (!isAndroid) {
    // Apple TV: power goes through send_command too (the same call that works in HA's Actions tool)
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
}

// Messages to the watch go out one at a time: wait for the watch's ACK before
// sending the next, and retry on NACK. Firing them on a timer can drop some.
var sendQueue = [];
var sending = false;

function pumpQueue() {
  if (sending || sendQueue.length === 0) return;
  sending = true;
  var item = sendQueue[0];
  Pebble.sendAppMessage(item.payload, function() {
    sendQueue.shift();
    sending = false;
    pumpQueue();
  }, function() {
    item.tries++;
    if (item.tries >= 5) sendQueue.shift();   // give up on this one
    sending = false;
    setTimeout(pumpQueue, 250);
  });
}

function enqueue(payload) {
  sendQueue.push({ payload: payload, tries: 0 });
  pumpQueue();
}

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
    }
  };
  xhr.send();
}

Pebble.addEventListener('ready', function(e) {
  activeEntityId = localStorage.getItem('active_tv_entity') || "";
  fetchTVListFromHA();
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
  if (e.payload['0'] !== undefined) {
    sendCommandToHA(e.payload['0']);
  } else if (e.payload['1'] !== undefined) {
    fetchTVListFromHA();
  } else if (e.payload['5'] !== undefined) {
    // User picked a TV from the watch menu
    activeEntityId = e.payload['5'];
    localStorage.setItem('active_tv_entity', activeEntityId);
  }
});
