import 'dart:convert';

import 'package:http/http.dart' as http;

const _sonarErr = -2147483648; // INT32_MIN: sonar read failed
const _dhtErr = -999; // DHT22 read failed

enum NodeStatus { normal, watch, alert, error }

class Node {
  final int id;
  final String name;
  final double lat, lng;
  Node(this.id, this.name, this.lat, this.lng);
  factory Node.fromJson(Map<String, dynamic> j) =>
      Node(j['node_id'], j['name'], (j['lat'] as num).toDouble(), (j['lng'] as num).toDouble());
}

class Reading {
  final int logTs, nodeId, levelMm;
  final double? tempC, humidityPct;
  final int? distanceMm;
  Reading(this.logTs, this.nodeId, this.levelMm, this.tempC, this.humidityPct, this.distanceMm);

  bool get sonarOk => levelMm != _sonarErr;

  factory Reading.fromJson(Map<String, dynamic> j) {
    double? dht(num v) => v == _dhtErr ? null : v.toDouble();
    final level = j['water_level_mm'] as int;
    return Reading(j['log_timestamp_s'], j['node_id'], level, dht(j['temperature_c']),
        dht(j['humidity_pct']), level == _sonarErr ? null : j['distance_mm']);
  }

  NodeStatus status(double warnMm, double dangerMm) {
    if (!sonarOk) return NodeStatus.error;
    if (levelMm >= dangerMm) return NodeStatus.alert;
    if (levelMm >= warnMm) return NodeStatus.watch;
    return NodeStatus.normal;
  }
}

class Status {
  final int nowS, uptimeS;
  final bool rtcOk, sdOk;
  Status(this.nowS, this.uptimeS, this.rtcOk, this.sdOk);
  factory Status.fromJson(Map<String, dynamic> j) =>
      Status(j['now_s'], j['uptime_s'], j['rtc_ok'], j['sd_ok']);
}

class ApiClient {
  String host; // "192.168.x.x", "jaga-master.local", or a full https:// address
  ApiClient(this.host);

  Future<dynamic> _get(String path) async {
    final res = await http.get(Uri.parse('${host.startsWith('http') ? host : 'http://$host'}$path')).timeout(const Duration(seconds: 5));
    if (res.statusCode != 200) throw Exception('HTTP ${res.statusCode}');
    return jsonDecode(res.body);
  }

  Future<List<T>> _list<T>(String path, T Function(Map<String, dynamic>) f) async =>
      [for (final j in await _get(path) as List) f(j as Map<String, dynamic>)];

  Future<Status> status() async => Status.fromJson(await _get('/api/status'));
  Future<List<Node>> nodes() => _list('/api/nodes', Node.fromJson);
  Future<List<Reading>> latest() => _list('/api/latest', Reading.fromJson);

  /// Oldest first (the API returns newest first).
  Future<List<Reading>> history(int node) async =>
      (await _list('/api/history?node=$node', Reading.fromJson)).reversed.toList();
}
