import 'dart:async';
import 'dart:io';

import 'package:flutter/painting.dart' show Color;
import 'package:flutter_test/flutter_test.dart';
import 'package:jaga_app/main.dart' show nodeColor;
import 'package:jaga_app/api.dart';
import 'package:jaga_app/lobby.dart';
import 'package:jaga_app/state.dart';

void main() {
  test('mergeNodeIds sorts and dedupes; nodeColor keeps 1..3 and wraps', () {
    expect(mergeNodeIds([3, 1], [], [8, 3, 2]), [1, 2, 3, 8]);
    expect(mergeNodeIds([], [], []), isEmpty);
    expect(nodeColor(1), const Color(0xFF2A7FFF));
    expect(nodeColor(2), const Color(0xFFE67E22));
    expect(nodeColor(3), const Color(0xFF8E44AD));
    expect(nodeColor(9), nodeColor(1));
  });

  test('parses sentinels, classifies status, alerts only on crossing', () {
    final ok = Reading.fromJson({
      'log_timestamp_s': 10, 'node_id': 3, 'sender_timestamp_s': 10, 'temperature_c': 27.5,
      'humidity_pct': 70.0, 'distance_mm': 100, 'distance_stddev_mm': 2, 'water_level_mm': 950,
    });
    expect(ok.status(700, 900), NodeStatus.alert);
    expect(Reading(1, 1, 750, null, null, null).status(700, 900), NodeStatus.watch);
    expect(Reading(1, 1, 100, null, null, null).status(700, 900), NodeStatus.normal);

    final bad = Reading.fromJson({
      'log_timestamp_s': 11, 'node_id': 1, 'sender_timestamp_s': 11, 'temperature_c': -999,
      'humidity_pct': -999, 'distance_mm': 0, 'distance_stddev_mm': 0, 'water_level_mm': -2147483648,
    });
    expect(bad.tempC, isNull);
    expect(bad.humidityPct, isNull);
    expect(bad.distanceMm, isNull);
    expect(bad.status(700, 900), NodeStatus.error);

    expect(crossedIntoAlert(NodeStatus.watch, NodeStatus.alert), isTrue);
    expect(crossedIntoAlert(null, NodeStatus.alert), isTrue);
    expect(crossedIntoAlert(NodeStatus.alert, NodeStatus.alert), isFalse);
    expect(crossedIntoAlert(NodeStatus.alert, NodeStatus.normal), isFalse);
  });
  healthTests();
  lobbyTests();
  errorTests();
  fixTests();
}

void fixTests() {
  Reading r(int ts, double? at, [int level = 600]) => Reading(ts, 1, level, null, null, null, receivedAt: at);
  test('isNewReading: reboot, duplicate, no receivedAt', () {
    expect(isNewReading(null, r(5, 1)), isTrue);
    expect(isNewReading(r(50000, 100), r(30, 110)), isTrue); // master rebooted, clock restarted
    expect(isNewReading(r(50, 100), r(50, 100)), isFalse); // duplicate
    expect(isNewReading(r(50, 100), r(60, 100)), isFalse); // same receivedAt wins over logTs
    expect(isNewReading(r(50, null), r(60, null)), isTrue);
    expect(isNewReading(r(50, null), r(50, null)), isFalse);
    expect(isNewReading(r(50, 100), r(60, null)), isTrue); // mixed: fall back to logTs
  });

  test('errorCode and ApiException', () {
    expect(errorCode('{"error":{"code":"master_unreachable","message":"x"}}'), 'master_unreachable');
    expect(errorCode('<html>'), isNull);
    expect(errorCode('{"error":"x"}'), isNull);
    expect(errorCode('[]'), isNull);
    expect(ApiException(503, 'master_unreachable').toString(), 'HTTP 503');
  });

  test('Node.fromJson without name', () {
    expect(Node.fromJson({'node_id': 2, 'lat': 1.5, 'lng': 2.5}).name, 'Node 2');
    expect(Node.fromJson({'node_id': 2, 'name': 'JAGA-02', 'lat': null, 'lng': null}).name, 'JAGA-02');
  });

  test('lastValid skips sonar faults', () {
    const f = -2147483648;
    expect(lastValid([r(1, 1, 800), r(2, 2, f)])!.levelMm, 800);
    expect(lastValid([r(1, 1, f)]), isNull);
    expect(lastValid([]), isNull);
  });
}

void errorTests() {
  test('join classifier and api_version gate', () {
    const noNet = 'No internet connection. Check your network and try again.';
    const noAnswer = "That server isn't answering.";
    const notJaga = "That address answered, but it isn't a JAGA server.";
    expect(joinFailure(const SocketException('Failed host lookup')), noNet);
    expect(joinFailure(const SocketException('Connection refused', osError: OSError('Connection refused', 111))), noAnswer);
    expect(joinFailure(TimeoutException('x')), noAnswer);
    expect(joinFailure(Exception('HTTP 502')), noAnswer);
    expect(joinFailure(const FormatException('<html>')), notJaga);
    final noNow = <String, dynamic>{'uptime_s': 1, 'rtc_ok': true, 'sd_ok': true};
    expect(() => Status.fromJson(noNow), throwsA(isA<TypeError>()));
    expect(joinFailure(TypeError()), notJaga);

    Status v(Map<String, dynamic> extra) =>
        Status.fromJson({'now_s': 1, 'uptime_s': 1, 'rtc_ok': true, 'sd_ok': true, ...extra});
    expect(v({}).apiVersion, 1); // master firmware: absent means 1
    expect(joinFailure(null, v({'api_version': 1})), isNull);
    expect(joinFailure(null, v({'api_version': 2})), tooNewMsg);
  });

  test('lost signal fires once per outage, only from Watch up', () {
    const live = NodeHealth.live, lost = NodeHealth.noSignal;
    expect(lostSignalFires(live, lost, 620, 600), isTrue);
    expect(lostSignalFires(lost, lost, 620, 600), isFalse); // same outage
    expect(lostSignalFires(live, lost, 500, 600), isFalse); // below Watch
    expect(lostSignalFires(live, lost, -2147483648, 600), isFalse); // sonar fault
    expect(lostSignalFires(null, lost, 620, 600), isFalse); // outage already on at launch
    expect(lostSignalFires(lost, live, 620, 600), isFalse);
    expect(lostSignalFires(live, lost, 620, 600), isTrue); // reported again, then lost again
  });
}

void healthTests() {
  test('node health and link state', () {
    Reading at(double t, [int level = 620]) => Reading(1, 1, level, null, null, null, receivedAt: t);
    expect(nodeHealth(at(100), 110), NodeHealth.live);
    expect(nodeHealth(at(100, -2147483648), 110), NodeHealth.fault);
    expect(nodeHealth(at(100), 279.9), NodeHealth.live);
    expect(nodeHealth(at(100), 280), NodeHealth.noSignal);
    expect(nodeHealth(at(100, -2147483648), 280), NodeHealth.noSignal);
    expect(nodeHealth(null, 280), NodeHealth.noData);
    expect(Status.fromJson({'now_s': 1, 'uptime_s': 1, 'rtc_ok': true, 'sd_ok': true}).masterOk, isTrue);

    expect(linkState(serverReachable: false, masterOk: false), LinkState.serverDown);
    expect(linkState(serverReachable: false, masterOk: true), LinkState.serverDown);
    expect(linkState(serverReachable: true, masterOk: false), LinkState.masterDown);
    expect(linkState(serverReachable: true, masterOk: true), LinkState.connected);
  });
}

void lobbyTests() {
  test('lobby: distance, nearest-site auto-join, address normalising', () {
    expect(distanceKm(1.865, 103.086, 1.865, 103.086), closeTo(0, 1e-9));
    expect(distanceKm(1.865, 103.086, 3.139, 101.687), closeTo(210, 3)); // UTHM -> Kuala Lumpur

    final uthm = Site('UTHM', 'https://a.example', 1.865, 103.086);
    final kl = Site('KL', 'https://b.example', 3.139, 101.687);
    expect(nearestSite([kl, uthm], 1.86, 103.08)!.name, 'UTHM');
    expect(nearestSite([kl, uthm], 1.86, 103.08, maxKm: 0.1), isNull); // too far to auto-join
    expect(nearestSite([Site('no gps', 'https://c.example')], 1.86, 103.08), isNull);

    expect(normalizeUrl('jaga.larmkx.com'), 'https://jaga.larmkx.com');
    expect(normalizeUrl('192.168.1.50'), 'http://192.168.1.50');
    expect(normalizeUrl('jaga-master.local/'), 'http://jaga-master.local');
    expect(normalizeUrl('localhost:8088'), 'http://localhost:8088');
    expect(normalizeUrl('https://x.example/'), 'https://x.example');
  });
}
