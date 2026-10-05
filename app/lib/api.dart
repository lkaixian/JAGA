import 'dart:async';
import 'dart:convert';
import 'dart:io';

import 'package:http/http.dart' as http;

const _sonarErr = -2147483648; // INT32_MIN: sonar read failed
const _dhtErr = -999; // DHT22 read failed

enum NodeStatus { normal, watch, alert, error }

/// Where the break is (docs/ERROR_HANDLING.md section 2).
enum LinkState { connected, masterDown, serverDown }

enum NodeHealth { live, fault, noSignal, noData }

const staleS = 180;
const supportedApiVersion = 1;
const tooNewMsg = 'This server needs a newer version of the app.';

/// Join failure text for an exception, or (no exception) for a reply that parsed. Null = joinable.
String? joinFailure(Object? e, [Status? st]) {
  if (e == null) return st != null && st.apiVersion > supportedApiVersion ? tooNewMsg : null;
  if (e is FormatException || e is TypeError) return "That address answered, but it isn't a JAGA server.";
  // ponytail: refusal is matched by message text; the OS error codes differ per platform
  if ((e is SocketException || e is http.ClientException) && !e.toString().toLowerCase().contains('refused')) {
    return 'No internet connection. Check your network and try again.';
  }
  return "That server isn't answering."; // timeout, HTTP error, connection refused
}

/// One notification per outage: only on the poll where a node at Watch or above goes silent.
bool lostSignalFires(NodeHealth? prev, NodeHealth now, int levelMm, double warnMm) =>
    now == NodeHealth.noSignal && prev != null && prev != NodeHealth.noSignal && levelMm >= warnMm;

/// Newest reading in [list] (oldest first) with a valid sonar value, or null.
Reading? lastValid(List<Reading> list) => list.reversed.where((r) => r.sonarOk).firstOrNull;

/// log_timestamp_s is the master's clock and can restart after a reboot, so prefer the server's receivedAt.
bool isNewReading(Reading? last, Reading r) => last == null
    ? true
    : (last.receivedAt != null && r.receivedAt != null)
        ? r.receivedAt! > last.receivedAt!
        : r.logTs > last.logTs;

/// HTTP error from the server; [code] is error.code from a JSON body, if any.
class ApiException implements Exception {
  final int statusCode;
  final String? code;
  ApiException(this.statusCode, [this.code]);
  @override
  String toString() => 'HTTP $statusCode';
}

/// error.code from a {"error":{"code":...}} body, or null for anything else.
String? errorCode(String body) {
  try {
    final c = ((jsonDecode(body) as Map)['error'] as Map)['code'];
    return c is String ? c : null;
  } catch (_) {
    return null;
  }
}

/// Server unreachable beats master unreachable beats connected.
LinkState linkState({required bool serverReachable, required bool masterOk}) =>
    !serverReachable ? LinkState.serverDown : (!masterOk ? LinkState.masterDown : LinkState.connected);

/// [nowS] is Unix time in seconds (the device clock), the same scale as [Reading.receivedAt];
/// the master's own now_s is not. Silence wins over a fault.
NodeHealth nodeHealth(Reading? r, num? nowS) {
  if (r == null) return NodeHealth.noData;
  final age = (r.receivedAt == null || nowS == null) ? 0 : nowS - r.receivedAt!;
  if (age >= staleS) return NodeHealth.noSignal;
  return r.sonarOk ? NodeHealth.live : NodeHealth.fault;
}

class Node {
  final int id;
  final String name;
  final double lat, lng;
  Node(this.id, this.name, this.lat, this.lng);
  factory Node.fromJson(Map<String, dynamic> j) =>
      Node(j['node_id'], j['name'] as String? ?? 'Node ${j['node_id']}', (j['lat'] as num?)?.toDouble() ?? double.nan, (j['lng'] as num?)?.toDouble() ?? double.nan);
}

class Reading {
  final int logTs, nodeId, levelMm;
  final double? tempC, humidityPct;
  final int? distanceMm;
  final double? receivedAt; // server unix seconds
  Reading(this.logTs, this.nodeId, this.levelMm, this.tempC, this.humidityPct, this.distanceMm,
      {this.receivedAt});

  bool get sonarOk => levelMm != _sonarErr;

  factory Reading.fromJson(Map<String, dynamic> j) {
    double? dht(num v) => v == _dhtErr ? null : v.toDouble();
    final level = j['water_level_mm'] as int;
    return Reading(j['log_timestamp_s'], j['node_id'], level, dht(j['temperature_c']),
        dht(j['humidity_pct']), level == _sonarErr ? null : j['distance_mm'],
        receivedAt: (j['received_at'] as num?)?.toDouble());
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
  final bool rtcOk, sdOk, masterOk; // masterOk is absent when talking to the master directly
  final int? masterAgeS;
  final int apiVersion; // absent on the master firmware: version 1
  Status(this.nowS, this.uptimeS, this.rtcOk, this.sdOk, [this.masterOk = true, this.masterAgeS, this.apiVersion = 1]);
  factory Status.fromJson(Map<String, dynamic> j) => Status(j['now_s'], j['uptime_s'], j['rtc_ok'],
      j['sd_ok'], j['master_ok'] ?? true, (j['master_age_s'] as num?)?.toInt(), (j['api_version'] as num?)?.toInt() ?? 1);
}

class ApiClient {
  String host; // "192.168.x.x", "jaga-master.local", or a full https:// address
  ApiClient(this.host);

  Future<dynamic> _get(String path) async {
    final res = await http.get(Uri.parse('${host.startsWith('http') ? host : 'http://$host'}$path')).timeout(const Duration(seconds: 5));
    if (res.statusCode != 200) throw ApiException(res.statusCode, errorCode(res.body));
    return jsonDecode(res.body);
  }

  Future<List<T>> _list<T>(String path, T Function(Map<String, dynamic>) f) async =>
      [for (final j in await _get(path) as List) f(j as Map<String, dynamic>)];

  /// 503 master_unreachable = the server is fine but has not reached the master yet: masterOk false.
  Future<Status> status() async {
    try {
      return Status.fromJson(await _get('/api/status'));
    } on ApiException catch (e) {
      if (e.code == 'master_unreachable') return Status(0, 0, false, false, false);
      rethrow;
    }
  }
  Future<List<Node>> nodes() => _list('/api/nodes', Node.fromJson);
  Future<List<Reading>> latest() => _list('/api/latest', Reading.fromJson);

  /// Oldest first (the API returns newest first).
  Future<List<Reading>> history(int node) async =>
      (await _list('/api/history?node=$node', Reading.fromJson)).reversed.toList();
}
