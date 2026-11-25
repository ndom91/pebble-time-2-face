// Fetches current weather from Open-Meteo (no API key) and sends it to the
// watch as TEMP (°C, rounded) and CODE (WMO weather code).
var REFRESH_MS = 30 * 60 * 1000;
var lastSent = 0;

function send(temp, code) {
  Pebble.sendAppMessage(
    { TEMP: temp, CODE: code },
    function () { lastSent = Date.now(); },
    function (e) { console.log('weather send failed: ' + JSON.stringify(e)); }
  );
}

function fetchWeather() {
  navigator.geolocation.getCurrentPosition(
    function (pos) {
      var url = 'https://api.open-meteo.com/v1/forecast' +
        '?latitude=' + pos.coords.latitude.toFixed(3) +
        '&longitude=' + pos.coords.longitude.toFixed(3) +
        '&current=temperature_2m,weather_code';
      var xhr = new XMLHttpRequest();
      xhr.onload = function () {
        try {
          var cur = JSON.parse(xhr.responseText).current;
          send(Math.round(cur.temperature_2m), cur.weather_code);
        } catch (err) {
          console.log('weather parse failed: ' + err);
        }
      };
      xhr.open('GET', url);
      xhr.send();
    },
    function (err) { console.log('location failed: ' + err.message); },
    { timeout: 15000, maximumAge: 10 * 60 * 1000 }
  );
}

Pebble.addEventListener('ready', fetchWeather);

// The watch asks for an update every 30 minutes.
Pebble.addEventListener('appmessage', function (e) {
  if (e.payload && e.payload.REQUEST !== undefined && Date.now() - lastSent > 60 * 1000) {
    fetchWeather();
  }
});
