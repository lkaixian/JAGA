import 'dart:convert';
import 'dart:io';

import 'package:flutter_test/flutter_test.dart';
import 'package:jaga_app/lobby.dart';
import 'package:jaga_app/state.dart';
import 'package:shared_preferences/shared_preferences.dart';

void main() {
  TestWidgetsFlutterBinding.ensureInitialized();
  HttpOverrides.global = null; // the test binding answers every request with 400 otherwise

  test('nodes with no readings get a fake stream, real ones are untouched', () async {
    final now = DateTime.now().millisecondsSinceEpoch / 1000;
    Map row(int node, int t) => {
          'log_timestamp_s': t, 'node_id': node, 'sender_timestamp_s': t, 'temperature_c': 25.0,
          'humidity_pct': 50.0, 'distance_mm': 800, 'distance_stddev_mm': 1, 'water_level_mm': 400,
          'received_at': now - t,
        };
    final server = await HttpServer.bind('127.0.0.1', 0);
    server.listen((req) {
      final p = req.uri.path;
      Object body;
      var code = 200;
      if (p == '/api/nodes') {
        body = [for (var i = 1; i <= 5; i++) {'node_id': i, 'name': 'Node $i', 'lat': null, 'lng': null}];
      } else if (p == '/api/latest') {
        body = [row(1, 5), row(2, 5)];
      } else if (p == '/api/history') {
        final n = int.parse(req.uri.queryParameters['node']!);
        body = n <= 2 ? [row(n, 5), row(n, 65)] : [];
      } else if (p == '/api/status') {
        body = {'now_s': 100, 'uptime_s': 10, 'rtc_ok': true, 'sd_ok': true, 'master_ok': true};
      } else {
        code = 404;
        body = {'error': {'code': 'nope'}};
      }
      req.response
        ..statusCode = code
        ..headers.contentType = ContentType.json
        ..write(jsonEncode(body))
        ..close();
    });

    SharedPreferences.setMockInitialValues({'host': 'http://127.0.0.1:${server.port}'});
    final s = AppState();
    try {
      await s.init().timeout(const Duration(seconds: 5));
    } catch (e) {
      // notification plugin is unavailable in a unit test; the prefs and host are already set
    }
    expect(await s.join(Site('Test', 'http://127.0.0.1:${server.port}')), isNull);
    await Future.delayed(const Duration(seconds: 2)); // join starts the first poll
    final snapshot = s.link.name;
    s.dispose();

    expect(snapshot, 'connected');
    for (final id in [3, 4, 5]) {
      expect(s.history[id], isNotEmpty, reason: 'node $id');
      expect(s.nodeById(id)!.name, endsWith('(sim)'));
    }
    for (final id in [1, 2]) {
      expect(s.history[id]!.length, 2, reason: 'node $id is real');
      expect(s.nodeById(id)!.name, isNot(endsWith('(sim)')));
    }
    await server.close(force: true);
  });
}
