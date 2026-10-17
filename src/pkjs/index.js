var activeEntityId = localStorage.getItem('active_tv_entity') || "";

function getDeviceCommand(entityId, cmd) {
  const isAndroid = entityId.includes("android") || entityId.includes("shield") || entityId.includes("fire");
  if (!isAndroid) {
    return cmd;
  } else {
    const androidMap = {
      'up': 'DPAD_UP', 'down': 'DPAD_DOWN', 'left': 'DPAD_LEFT',
      'right': 'DPAD_RIGHT', 'select': 'DPAD_CENTER', 'menu': 'BACK'
    };
    return androidMap[cmd] || cmd;
  }
}

function sendCommandToHA(commandName) {
  var haUrl = localStorage.getItem('ha_url');
  var haToken = localStorage.getItem('ha_token');
  if (!haUrl || !haToken || !activeEntityId) return;

  if(haUrl.endsWith('/')) haUrl = haUrl.slice(0, -1);
  const url = haUrl + "/api/services/remote/send_command";
  
  var xhr = new XMLHttpRequest();
  xhr.open('POST', url);
  xhr.setRequestHeader("Authorization", "Bearer " + haToken);
  xhr.setRequestHeader("Content-Type", "application/json");
  xhr.send(JSON.stringify({
    entity_id: activeEntityId,
    command: getDeviceCommand(activeEntityId, commandName)
  }));
}

function fetchTVListFromHA() {
  var haUrl = localStorage.getItem('ha_url');
  var haToken = localStorage.getItem('ha_token');
  if (!haUrl || !haToken) return;

  if(haUrl.endsWith('/')) haUrl = haUrl.slice(0, -1);
  const url = haUrl + "/api/states";

  var xhr = new XMLHttpRequest();
  xhr.open('GET', url);
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

// Uses Joshua Galea's verified, whitelisted Pebble settings domain path
Pebble.addEventListener('showConfiguration', function(e) {
  Pebble.openURL('https://github.io');
});

Pebble.addEventListener('webkitclosedconfiguration', function(e) {
  if (e.response) {
    try {
      var configData = JSON.parse(decodeURIComponent(e.response));
      // The official page saves keys as 'url' and 'token'
      if (configData.url && configData.token) {
        localStorage.setItem('ha_url', configData.url);
        localStorage.setItem('ha_token', configData.token);
        console.log("Credentials securely committed to app cache!");
        
        // Immediately look up the TV list from your server
        fetchTVListFromHA();
      }
    } catch(err) {
      console.log("Error reading data dictionary: " + err);
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
