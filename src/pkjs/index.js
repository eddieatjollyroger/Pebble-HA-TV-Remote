var activeEntityId = localStorage.getItem('active_tv_entity') || "";

function getDeviceCommand(entityId, cmd) {
  const isAndroid = entityId.includes("android") || entityId.includes("shield") || entityId.includes("fire");
  if (!isAndroid) {
    return cmd; // Returns raw navigation strings: up, down, left, right, select, menu, volume_up, volume_down
  } else {
    const androidMap = {
      'up': 'DPAD_UP', 'down': 'DPAD_DOWN', 'left': 'DPAD_LEFT',
      'right': 'DPAD_RIGHT', 'select': 'DPAD_CENTER', 'menu': 'BACK',
      'volume_up': 'VOLUME_UP', 'volume_down': 'VOLUME_DOWN'
    };
    return androidMap[cmd] || cmd;
  }
}

function sendCommandToHA(commandName) {
  var haUrl = localStorage.getItem('ha_url');
  var haToken = localStorage.getItem('ha_token');
  
  if (!haUrl || !haToken || !activeEntityId) {
    console.log("Cannot send remote command: Configuration variables are missing.");
    return;
  }

  if (haUrl.endsWith('/')) haUrl = haUrl.slice(0, -1);
  
  // Exact REST endpoint used to execute native actions
  const url = haUrl + "/api/services/remote/send_command";
  
  var xhr = new XMLHttpRequest();
  xhr.open('POST', url, true);
  xhr.setRequestHeader("Authorization", "Bearer " + haToken);
  xhr.setRequestHeader("Content-Type", "application/json");

  // FIXED PAYLOAD STRUCTURAL ALLIGNMENT:
  // Mirrors your working Home Assistant layout exactly.
  // Both keys exist together on the root tier, with 'command' wrapped inside an array list string.
  var payload = {
    "entity_id": activeEntityId,
    "command": [getDeviceCommand(activeEntityId, commandName)]
  };

  xhr.onreadystatechange = function () {
    if (xhr.readyState === 4) {
      if (xhr.status >= 200 && xhr.status < 300) {
        console.log("Success! Dispatched command: " + commandName + " to " + activeEntityId);
      } else {
        console.log("Home Assistant API error. Status: " + xhr.status + " - Response: " + xhr.responseText);
      }
    }
  };

  xhr.send(JSON.stringify(payload));
}

function fetchTVListFromHA() {
  var haUrl = localStorage.getItem('ha_url');
  var haToken = localStorage.getItem('ha_token');
  if (!haUrl || !haToken) return;

  if (haUrl.endsWith('/')) haUrl = haUrl.slice(0, -1);
  const url = haUrl + "/api/states";

  var xhr = new XMLHttpRequest();
  xhr.open('GET', url, true);
  xhr.setRequestHeader("Authorization", "Bearer " + haToken);
  
  xhr.onload = function() {
    if (xhr.status === 200) {
      var states = JSON.parse(xhr.responseText);
      var remotes = states.filter(function(item) {
        return item.entity_id.startsWith('remote.');
      });

      Pebble.sendAppMessage({ '2': remotes.length });

      remotes.forEach(function(remote, index) {
        setTimeout(function() {
          Pebble.sendAppMessage({
            '3': remote.attributes.friendly_name || remote.entity_id,
            '4': remote.entity_id
          });
        }, index * 150);
      });
    }
  };
  xhr.send();
}

Pebble.addEventListener('ready', function(e) {
  fetchTVListFromHA();
  // Dynamically remember the last device selection chosen from your list menu view!
  activeEntityId = localStorage.getItem('active_tv_entity') || "remote.quarto_tv_quarto_tv";
});


Pebble.addEventListener('showConfiguration', function(e) {
  var currentUrl = localStorage.getItem('ha_url') || '';
  var currentToken = localStorage.getItem('ha_token') || '';
  var returnUrl = e.return_url || 'pebblejs://close#';
  var myGitHubPage = 'https://eddieatjollyroger.github.io/Pebble-HA-TV-Remote/';
  
  var finalUrl = myGitHubPage + 
                 '?url=' + encodeURIComponent(currentUrl) + 
                 '&token=' + encodeURIComponent(currentToken) + 
                 '&return_to=' + encodeURIComponent(returnUrl);
                 
  Pebble.openURL(finalUrl);
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
      console.log("JSON Parse Error: " + err);
    }
  }
});

Pebble.addEventListener('appmessage', function(e) {
  if (e.payload) {
    if (e.payload['0'] !== undefined) {
      sendCommandToHA(e.payload['0']);
    } else if (e.payload['1'] !== undefined) {
      fetchTVListFromHA();
    } else if (e.payload['5'] !== undefined) {
      activeEntityId = e.payload['5'];
      localStorage.setItem('active_tv_entity', activeEntityId);
    }
  }
});
