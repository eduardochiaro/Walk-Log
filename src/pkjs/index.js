var timelinejs = require('pebble-timeline-js');

Pebble.addEventListener('ready', function () {
  console.log('PebbleKit JS ready');
});


Pebble.addEventListener('appmessage', function (e) {
  console.log('Received message from watch');

  var startTime = e.payload.SESSION_START_TIME;
  var deletePin = e.payload.DELETE_PIN_START_TIME;

  if (deletePin) {
    deleteTimelinePin(deletePin);
  } else if (startTime) {
    var endTime   = e.payload.SESSION_END_TIME;
    var steps     = e.payload.SESSION_STEPS;
    var elapsed   = e.payload.SESSION_ELAPSED;
    insertTimelinePin(startTime, endTime, steps, elapsed);
  }
});

function deleteTimelinePin(startTime) {
  var pinId = 'walk-log-' + startTime;

  console.log('deleting timeline pin with id: ' + pinId);

  timelinejs.deleteUserPin({
    id: pinId
  }, function(responseText) {
    console.log('Timeline pin delete response: ' + responseText);
  });
}

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

  timelinejs.insertUserPin(pin, function(responseText) {
    console.log('Result: ' + responseText);
  });
}
