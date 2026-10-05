import 'dart:async';

import 'package:flutter/foundation.dart';
import 'package:flutter_local_notifications/flutter_local_notifications.dart';
import 'package:shared_preferences/shared_preferences.dart';

import 'api.dart';

const maxPoints = 90;

/// True only on the poll where a node first reaches alert, so a node that
/// stays in alert doesn't re-notify every 10s.
bool crossedIntoAlert(NodeStatus? prev, NodeStatus next) =>
    next == NodeStatus.alert && prev != NodeStatus.alert;

class AppState extends ChangeNotifier {
  late final SharedPreferences _prefs;
  final _notifier = FlutterLocalNotificationsPlugin();
  late ApiClient _api;
  Timer? _timer;

  String host = 'jaga-master.local';
  double warnMm = 700, dangerMm = 900, mountMm = 1000;

  List<Node> nodes = [];
  final Map<int, List<Reading>> history = {1: [], 2: [], 3: []}; // oldest first
  Status? serverStatus;
  DateTime? lastOk;
  bool online = false;
  bool _historyLoaded = false;
  final Map<int, NodeStatus> _lastStatus = {};

  Future<void> init() async {
    _prefs = await SharedPreferences.getInstance();
    host = _prefs.getString('host') ?? host;
    warnMm = _prefs.getDouble('warn') ?? warnMm;
    dangerMm = _prefs.getDouble('danger') ?? dangerMm;
    mountMm = _prefs.getDouble('mount') ?? mountMm;
    _api = ApiClient(host);

    await _notifier.initialize(
        settings: const InitializationSettings(
            android: AndroidInitializationSettings('@mipmap/ic_launcher'),
            iOS: DarwinInitializationSettings()));
    await _notifier
        .resolvePlatformSpecificImplementation<AndroidFlutterLocalNotificationsPlugin>()
        ?.requestNotificationsPermission();

    poll();
    _timer = Timer.periodic(const Duration(seconds: 10), (_) => poll());
  }

  @override
  void dispose() {
    _timer?.cancel();
    super.dispose();
  }

  Future<void> saveSettings(String newHost, double warn, double danger, double mount) async {
    final hostChanged = newHost != host;
    host = newHost;
    warnMm = warn;
    dangerMm = danger;
    mountMm = mount;
    await _prefs.setString('host', host);
    await _prefs.setDouble('warn', warn);
    await _prefs.setDouble('danger', danger);
    await _prefs.setDouble('mount', mount);
    if (hostChanged) {
      _api = ApiClient(host);
      nodes = [];
      history.updateAll((_, _) => []);
      _historyLoaded = false;
      _lastStatus.clear();
      poll();
    }
    notifyListeners();
  }

  Node? nodeById(int id) => nodes.where((n) => n.id == id).firstOrNull;
  Reading? latestOf(int id) => history[id]!.lastOrNull;

  /// Seconds since the node's newest reading, by the master's own clock.
  int? ageS(int id) {
    final r = latestOf(id), s = serverStatus;
    if (r == null || s == null) return null;
    return s.nowS - r.logTs;
  }

  Future<void> poll() async {
    try {
      if (nodes.isEmpty) nodes = await _api.nodes();
      if (!_historyLoaded) {
        for (final id in history.keys) {
          history[id] = await _api.history(id);
        }
        _historyLoaded = true;
      }
      serverStatus = await _api.status();
      for (final r in await _api.latest()) {
        final list = history[r.nodeId];
        if (list == null || (list.isNotEmpty && r.logTs <= list.last.logTs)) continue;
        list.add(r);
        if (list.length > maxPoints) list.removeAt(0);
      }
      online = true;
      lastOk = DateTime.now();
      _checkAlerts();
    } catch (_) {
      online = false; // keep the last data on screen
    }
    notifyListeners();
  }

  void _checkAlerts() {
    for (final id in history.keys) {
      final r = latestOf(id);
      if (r == null) continue;
      final s = r.status(warnMm, dangerMm);
      if (crossedIntoAlert(_lastStatus[id], s)) {
        final name = nodeById(id)?.name ?? 'Node $id';
        _notifier.show(
            id: id,
            title: 'Flood alert: $name',
            body: 'Water level ${(r.levelMm / 1000).toStringAsFixed(2)} m',
            notificationDetails: const NotificationDetails(
                android: AndroidNotificationDetails('alerts', 'Flood alerts',
                    importance: Importance.high, priority: Priority.high),
                iOS: DarwinNotificationDetails()));
      }
      _lastStatus[id] = s;
    }
  }
}
