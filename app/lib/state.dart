import 'dart:async';
import 'dart:convert';
import 'dart:math';

import 'package:flutter/widgets.dart';
import 'package:flutter_local_notifications/flutter_local_notifications.dart';
import 'package:shared_preferences/shared_preferences.dart';

import 'api.dart';
import 'lobby.dart';

const maxPoints = 90;

/// True only on the poll where a node first reaches alert, so a node that
/// stays in alert doesn't re-notify every 10s.
bool crossedIntoAlert(NodeStatus? prev, NodeStatus next) =>
    next == NodeStatus.alert && prev != NodeStatus.alert;

/// Sorted unique union of node ids from every source.
List<int> mergeNodeIds(Iterable<int> known, Iterable<int> fromNodes, Iterable<int> fromLatest) =>
    {...known, ...fromNodes, ...fromLatest}.toList()..sort();

class AppState extends ChangeNotifier with WidgetsBindingObserver {
  late final SharedPreferences _prefs;
  final _notifier = FlutterLocalNotificationsPlugin();
  ApiClient? _apiClient;
  ApiClient get _api => _apiClient!;
  Timer? _timer;

  String? host; // null = not joined to a server yet: show the lobby
  String siteName = 'JAGA';
  List<Site> recent = [];
  double warnMm = 700, dangerMm = 900, mountMm = 1000;

  List<Node> nodes = [];
  final Map<int, List<Reading>> history = {}; // node id (ascending) -> readings, oldest first
  Status? serverStatus;
  DateTime? lastOk;
  LinkState link = LinkState.connected;
  int _fails = 0;
  bool _paused = false;
  final Set<int> _historyLoaded = {};
  final Map<int, NodeStatus> _lastStatus = {};
  final Map<int, NodeHealth> _lastHealth = {};
  bool notificationsOff = false;

  bool get serverTooNew => (serverStatus?.apiVersion ?? 1) > supportedApiVersion;
  String? get cachedDirectory => _prefs.getString('directory');
  Future<void> saveDirectory(String json) => _prefs.setString('directory', json);

  Future<void> init() async {
    _prefs = await SharedPreferences.getInstance();
    host = _prefs.getString('host');
    siteName = _prefs.getString('siteName') ?? siteName;
    recent = [
      for (final j in jsonDecode(_prefs.getString('recent') ?? '[]') as List) Site.fromJson(j as Map<String, dynamic>)
    ];
    warnMm = _prefs.getDouble('warn') ?? warnMm;
    dangerMm = _prefs.getDouble('danger') ?? dangerMm;
    mountMm = _prefs.getDouble('mount') ?? mountMm;

    await _notifier.initialize(
        settings: const InitializationSettings(
            android: AndroidInitializationSettings('@mipmap/ic_launcher'),
            iOS: DarwinInitializationSettings()));
    await _notifier
        .resolvePlatformSpecificImplementation<AndroidFlutterLocalNotificationsPlugin>()
        ?.requestNotificationsPermission();
    await checkNotifications();

    WidgetsBinding.instance.addObserver(this);
    if (host != null) _start();
  }

  /// Updates [notificationsOff] from the OS permission; Settings and resume call this.
  Future<void> checkNotifications() async {
    bool? on;
    try {
      on = await _notifier.resolvePlatformSpecificImplementation<AndroidFlutterLocalNotificationsPlugin>()?.areNotificationsEnabled() ??
          (await _notifier.resolvePlatformSpecificImplementation<IOSFlutterLocalNotificationsPlugin>()?.checkPermissions())?.isEnabled;
    } catch (e) {
      debugPrint('notification check failed: $e');
    }
    if (on != null && notificationsOff == on) {
      notificationsOff = !on;
      notifyListeners();
    }
  }

  void _start() {
    _apiClient = ApiClient(host!);
    _fails = 0;
    poll();
  }

  /// One-shot timer: 10 s when healthy, 10/20/30 s (cap) after failures.
  void _schedule() {
    _timer?.cancel();
    if (host == null || _paused) return;
    _timer = Timer(Duration(seconds: 10 * min(max(_fails, 1), 3)), poll);
  }

  @override
  void didChangeAppLifecycleState(AppLifecycleState state) {
    if (state == AppLifecycleState.resumed) checkNotifications();
    if (state == AppLifecycleState.paused) {
      _paused = true;
      _timer?.cancel();
    } else if (state == AppLifecycleState.resumed && _paused) {
      _paused = false;
      if (host != null) poll();
    }
  }

  /// When the master's last reading arrived, for the banner.
  DateTime? get masterLastAt {
    final a = serverStatus?.masterAgeS;
    return lastOk == null || a == null ? lastOk : lastOk!.subtract(Duration(seconds: a));
  }

  void _reset() {
    nodes = [];
    history.clear();
    serverStatus = null;
    link = LinkState.connected;
    _historyLoaded.clear();
    _lastStatus.clear();
    _lastHealth.clear();
  }

  /// Joins [site] after checking it answers. Returns an error message, or null on success.
  Future<String?> join(Site site) async {
    final Status st;
    try {
      st = await ApiClient(site.url).status();
    } catch (e) {
      debugPrint('join failed: $e');
      return joinFailure(e);
    }
    final bad = joinFailure(null, st);
    if (bad != null) return bad;
    host = site.url;
    siteName = site.name;
    recent = [site, ...recent.where((r) => r.url != site.url)].take(5).toList();
    await _prefs.setString('host', site.url);
    await _prefs.setString('siteName', site.name);
    await _prefs.setString('recent', jsonEncode([for (final r in recent) r.toJson()]));
    _reset();
    _start();
    notifyListeners();
    return null;
  }

  /// Back to the lobby; keeps the recent list so it doesn't auto-join again.
  Future<void> leave() async {
    _timer?.cancel();
    host = null;
    await _prefs.remove('host');
    _reset();
    notifyListeners();
  }

  @override
  void dispose() {
    WidgetsBinding.instance.removeObserver(this);
    _timer?.cancel();
    super.dispose();
  }

  Future<void> saveSettings(double warn, double danger, double mount) async {
    warnMm = warn;
    dangerMm = danger;
    mountMm = mount;
    await _prefs.setDouble('warn', warn);
    await _prefs.setDouble('danger', danger);
    await _prefs.setDouble('mount', mount);
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
      final latest = await _api.latest();
      final ids = mergeNodeIds(history.keys, nodes.map((n) => n.id), latest.map((r) => r.nodeId));
      final old = Map.of(history);
      history
        ..clear()
        ..addEntries([for (final id in ids) MapEntry(id, old[id] ?? [])]);
      for (final id in ids) {
        if (_historyLoaded.contains(id)) continue;
        history[id] = await _api.history(id);
        _historyLoaded.add(id);
      }
      serverStatus = await _api.status();
      for (final r in latest) {
        final list = history[r.nodeId];
        if (list == null || (list.isNotEmpty && r.logTs <= list.last.logTs)) continue;
        list.add(r);
        if (list.length > maxPoints) list.removeAt(0);
      }
      _fails = 0;
      link = linkState(serverReachable: true, masterOk: serverStatus!.masterOk);
      lastOk = DateTime.now();
      _checkAlerts();
    } catch (e) {
      debugPrint('poll failed (${e is TypeError || e is FormatException ? 'bad reply' : 'network'}): $e');
      _fails++;
      link = LinkState.serverDown; // keep the last data on screen
    }
    notifyListeners();
    _schedule();
  }

  void _show(int id, String title, String body) => _notifier.show(
      id: id,
      title: title,
      body: body,
      notificationDetails: const NotificationDetails(
          android: AndroidNotificationDetails('alerts', 'Flood alerts', importance: Importance.high, priority: Priority.high),
          iOS: DarwinNotificationDetails()));

  void _checkAlerts() {
    final nowS = DateTime.now().millisecondsSinceEpoch / 1000;
    for (final id in history.keys) {
      final r = latestOf(id);
      if (r == null) continue;
      final name = nodeById(id)?.name ?? 'Node $id';
      final s = r.status(warnMm, dangerMm);
      if (crossedIntoAlert(_lastStatus[id], s)) {
        _show(id, 'Flood alert: $name', 'Water level ${(r.levelMm / 1000).toStringAsFixed(2)} m');
      }
      _lastStatus[id] = s;
      final h = nodeHealth(r, nowS);
      if (lostSignalFires(_lastHealth[id], h, r.levelMm, warnMm)) {
        _show(100 + id, 'Lost signal: $name',
            'No report for ${((nowS - r.receivedAt!) / 60).floor()} min. Last level ${(r.levelMm / 1000).toStringAsFixed(2)} m.');
      }
      _lastHealth[id] = h;
    }
  }
}
