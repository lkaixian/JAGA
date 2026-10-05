import 'package:flutter_test/flutter_test.dart';
import 'package:jaga_app/api.dart';
import 'package:jaga_app/state.dart';

void main() {
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
}
