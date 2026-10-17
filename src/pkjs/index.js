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

// FIXED: Listen to the correct native webviewclosed keyword event listener hook
Pebble.addEventListener('webviewclosed', function(e) {
  if (e.response) {
    try {
      var configData = JSON.parse(decodeURIComponent(e.response));
      
      if (configData.ha_url && configData.ha_token) {
        localStorage.setItem('ha_url', configData.ha_url);
        localStorage.setItem('ha_token', configData.ha_token);
        console.log("Credentials saved directly into LocalStorage container cache!");
        
        // Auto-refresh the configuration pipeline right away
        fetchTVListFromHA();
      }
    } catch(err) {
      console.log("JSON Configuration decode exception error: " + err);
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
