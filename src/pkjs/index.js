// PebbleKit JS – receives session data from the watch and pushes a timeline pin

Pebble.addEventListener('ready', function () {
  console.log('PebbleKit JS ready');
});

Pebble.addEventListener('appmessage', function (e) {
  console.log('Received session from watch');

  var startTime = e.payload.SessionStartTime;
  var endTime   = e.payload.SessionEndTime;
  var steps     = e.payload.SessionSteps;
  var elapsed   = e.payload.SessionElapsed;

  if (startTime) {
    insertTimelinePin(startTime, endTime, steps, elapsed);
  }
});

function insertTimelinePin(startTime, endTime, steps, elapsed) {
  var mins = Math.floor(elapsed / 60);
  var secs = elapsed % 60;
  var durationStr = mins + 'm ' + secs + 's';

  var pin = {
    id:       'walk-log-' + startTime,
    time:     new Date(startTime * 1000).toISOString(),
    duration: Math.max(1, Math.ceil(elapsed / 60)),
    layout: {
      type:     'genericPin',
      title:    'Nice walk!',
      subtitle: durationStr + ' | ' + steps + ' steps',
      body:     'Duration: ' + durationStr + '\nSteps: ' + steps,
      tinyIcon: 'system://images/TIMELINE_SPORTS'
    }
  };

  Pebble.getTimelineToken(
    function (token) {
      var url = 'https://timeline-api.getpebble.com/v1/user/pins/' + pin.id;
      var xhr = new XMLHttpRequest();
      xhr.open('PUT', url, true);
      xhr.setRequestHeader('Content-Type', 'application/json');
      xhr.setRequestHeader('X-User-Token', token);
      xhr.onload = function () {
        console.log('Timeline pin response: ' + xhr.status);
      };
      xhr.onerror = function () {
        console.log('Timeline pin request failed');
      };
      xhr.send(JSON.stringify(pin));
    },
    function (error) {
      console.log('Error getting timeline token: ' + error);
    }
  );
}
