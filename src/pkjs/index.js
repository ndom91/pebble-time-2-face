// Settings page (Clay) and weather. Weather comes from Open-Meteo (no API
// key) and goes to the watch as TEMP_X10 (°C × 10), CODE (WMO weather code)
// and UPDATED (when it was fetched); the watch converts to °F itself.
var Clay = require('@rebble/clay');
var clayConfig = require('./config');
// Clay registers the settings page handlers itself.
new Clay(clayConfig); // eslint-disable-line no-new

var MAX_AGE_MS = 30 * 60 * 1000;
var CACHE_KEY = 'weather';

function readJson(key) {
  try {
    return JSON.parse(localStorage.getItem(key)) || null;
  } catch (e) {
    return null;
  }
}

// Clay keeps the saved settings in localStorage under 'clay-settings'.
function weatherEnabled() {
  var settings = readJson('clay-settings');
  return !settings || settings.SHOW_WEATHER !== false;
}

function send(weather) {
  Pebble.sendAppMessage(
    { TEMP_X10: weather.tempX10, CODE: weather.code, UPDATED: Math.floor(weather.at / 1000) },
    null,
    function (e) { console.log('weather send failed: ' + JSON.stringify(e)); }
  );
}

function fetchWeather() {
  navigator.geolocation.getCurrentPosition(
    function (pos) {
      var url = 'https://api.open-meteo.com/v1/forecast' +
        '?latitude=' + pos.coords.latitude.toFixed(2) +
        '&longitude=' + pos.coords.longitude.toFixed(2) +
        '&current=temperature_2m,weather_code';
      var xhr = new XMLHttpRequest();
      xhr.onload = function () {
        try {
          var cur = JSON.parse(xhr.responseText).current;
          var weather = {
            tempX10: Math.round(cur.temperature_2m * 10),
            code: cur.weather_code,
            at: Date.now()
          };
          localStorage.setItem(CACHE_KEY, JSON.stringify(weather));
          send(weather);
        } catch (err) {
          console.log('weather parse failed: ' + err);
        }
      };
      xhr.open('GET', url);
      xhr.send();
    },
    function (err) { console.log('location failed: ' + err.message); },
    // A coarse, recent fix is plenty for weather and spares the phone's GPS.
    { enableHighAccuracy: false, timeout: 15000, maximumAge: 60 * 60 * 1000 }
  );
}

// Sends the cached reading if it is fresh enough, otherwise fetches a new one.
function updateWeather() {
  if (!weatherEnabled()) return;
  var cached = readJson(CACHE_KEY);
  if (cached && Date.now() - cached.at < MAX_AGE_MS) {
    send(cached);
  } else {
    fetchWeather();
  }
}

Pebble.addEventListener('ready', updateWeather);

// The watch asks when its reading is older than 30 minutes.
Pebble.addEventListener('appmessage', function (e) {
  if (e.payload && e.payload.REQUEST !== undefined) updateWeather();
});
