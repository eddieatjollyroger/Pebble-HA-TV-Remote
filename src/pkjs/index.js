var activeEntityId = localStorage.getItem('active_tv_entity') || "";

function getDeviceCommand(entityId, cmd) {
  const isAndroid = entityId.includes("android") || entityId.includes("shield") || entityId.includes("fire");
  if (!isAndroid) {
    const appleMap = { 'power': 'home', 'home': 'home_hold' };
    return appleMap[cmd] || cmd;
  } else {
    const androidMap = {
      'up': 'DPAD_UP', 'down': 'DPAD_DOWN', 'left': 'DPAD_LEFT',
      'right': 'DPAD_RIGHT', 'select': 'DPAD_CENTER', 'menu': 'BACK',
      'volume_up': 'VOLUME_UP', 'volume_down': 'VOLUME_DOWN',
      'power': 'POWER', 'home': 'HOME'
    };
    return androidMap[cmd] || cmd;
  }
}

function sendCommandToHA(commandName) {
  var haUrl = localStorage.getItem('ha_url');
  var haToken = localStorage.getItem('ha_token');
  if (!haUrl || !haToken || !activeEntityId) return;

  if (haUrl.endsWith('/')) haUrl = haUrl.slice(0, -1);
  const url = haUrl + "/api/services/remote/send_command";
  
  var xhr = new XMLHttpRequest();
  xhr.open('POST', url, true);
  xhr.setRequestHeader("Authorization", "Bearer " + haToken);
  xhr.setRequestHeader("Content-Type", "application/json");

  var payload = {
    "entity_id": activeEntityId,
    "command": [getDeviceCommand(activeEntityId, commandName)]
  };
  xhr.send(JSON.stringify(payload));
}

function fetchTVListFromHA() {
  var haUrl = localStorage.getItem('ha_url');
  var haToken = localStorage.getItem('ha_token');
  if (!haUrl || !haToken) return;
  if (haUrl.endsWith('/')) haUrl = haUrl.slice(0, -1);

  var xhr = new XMLHttpRequest();
  xhr.open('GET', haUrl + "/api/states", true);
  xhr.setRequestHeader("Authorization", "Bearer " + haToken);
  
  xhr.onload = function() {
    if (xhr.status === 200) {
      var states = JSON.parse(xhr.responseText);
      var remotes = states.filter(function(item) { return item.entity_id.startsWith('remote.'); });
      
      Pebble.sendAppMessage({ '2': remotes.length });

      var activeFriendlyName = "";

      remotes.forEach(function(remote, index) {
        if (remote.entity_id === activeEntityId) {
          activeFriendlyName = remote.attributes.friendly_name || remote.entity_id;
        }
        setTimeout(function() {
          Pebble.sendAppMessage({
            '3': remote.attributes.friendly_name || remote.entity_id,
            '4': remote.entity_id
          });
        }, index * 120);
      });
      
      // Phone automatically sends the display label down to the watch on launch
      if (activeFriendlyName) {
        setTimeout(function() {
          Pebble.sendAppMessage({ '5': activeFriendlyName });
        }, (remotes.length + 1) * 120);
      }
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
  Pebble.openURL('https://eddieatjollyroger.github.io/Pebble-HA-TV-Remote' + encodeURIComponent(currentUrl) + '&token=' + encodeURIComponent(currentToken) + '&return_to=' + encodeURIComponent(returnUrl));
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
    } catch(err) {}
  }
});

Pebble.addEventListener('appmessage', function(e) {
  if (e.payload) {
    if (e.payload['0'] !== undefined) sendCommandToHA(e.payload['0']);
    else if (e.payload['1'] !== undefined) fetchTVListFromHA();
    else if (e.payload['5'] !== undefined) { 
      // User picked a new TV from list view
      activeEntityId = e.payload['5']; 
      localStorage.setItem('active_tv_entity', activeEntityId); 
      fetchTVListFromHA(); // Auto-refresh everything
    }
  }
});
