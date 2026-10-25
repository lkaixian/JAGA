import 'dart:async';
import 'dart:convert';
import 'dart:math';

import 'package:flutter/material.dart' show ScaffoldMessengerState, SnackBar, ThemeMode;
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

/// Nodes with no readings on the server get a fake storm stream in the app (never stored, never alerted on).
const _simNodes = {
  1: ('Tasik G3', 1.8630, 103.0840),
  2: ('Infront FKAAB', 1.8640, 103.0850),
  3: ('Infront FSKTM', 1.8650, 103.0860),
  4: ('Infront FPTP', 1.8660, 103.0870),
  5: ('Infront FKEE', 1.8670, 103.0880),
};
final _rng = Random();

/// Rainfall in mm/h, a storm every 3 hours; [seed] delays it per node.
double _storm(double t, double seed) {
  final x = t - 25 - seed * 7;
  return max(0, 68 * exp(-pow(x / 16, 2)) + 3 * max(0, sin((t + seed * 20) / 28)) + (_rng.nextDouble() - .5) * 2.5);
}

class AppState extends ChangeNotifier with WidgetsBindingObserver {
  late final SharedPreferences _prefs;
  final _notifier = FlutterLocalNotificationsPlugin();
  ApiClient? _apiClient;
  ApiClient get _api => _apiClient!;
  Timer? _timer;

  String? host; // null = not joined to a server yet: show the lobby
  String siteName = 'JAGA';
  List<Site> recent = [];
  final messenger = GlobalKey<ScaffoldMessengerState>();
  Map<String, dynamic> _levels = {}; // host -> LevelPrefs json
  ServerConfig? config; // what the joined server publishes; null = nothing
  bool levelsNote = false; // server changed its levels while protection kept the user's
  DateTime? _cfgAt;

  LevelPrefs get levelPrefs => host == null || _levels[host] == null ? LevelPrefs() : LevelPrefs.fromJson(_levels[host]);
  Levels get levels => activeLevels(levelPrefs, config);
  double get warnMm => levels.$1.toDouble();
  double get dangerMm => levels.$2.toDouble();
  double get mountMm => levels.$3.toDouble();

  Future<void> _saveLevels(LevelPrefs p) {
    _levels[host!] = p.toJson();
    notifyListeners();
    return _prefs.setString('levels', jsonEncode(_levels));
  }

  Future<void> setCustom(bool on) {
    final p = levelPrefs;
    if (on) p.mine = levels; // start from what is active now
    p.custom = on;
    if (!on) levelsNote = false;
    return _saveLevels(p);
  }

  Future<void> setProtect(bool on) => _saveLevels(levelPrefs..protect = on);
  Future<void> saveCustom(Levels l) => _saveLevels(levelPrefs..mine = l);

  /// At most once per 10 minutes unless [force]; a failure keeps the last known config.
  Future<void> fetchConfig({bool force = false}) async {
    final h = host, now = DateTime.now();
    if (h == null || (!force && _cfgAt != null && now.difference(_cfgAt!) < const Duration(minutes: 10))) return;
    _cfgAt = now;
    try {
      final c = await _api.config();
      if (host != h) return;
      config = c;
      if (c == null) return;
      final p = levelPrefs;
      switch (levelAction(custom: p.custom, protect: p.protect, lastRev: p.lastRev, newRev: c.rev)) {
        case LevelAction.overwriteCustom:
          p.mine = c.levels;
          messenger.currentState?.showSnackBar(SnackBar(content: Text('Levels updated from $siteName.')));
        case LevelAction.keepAndNote:
          levelsNote = p.lastRev != null; // first sight of a server is not a change
        case LevelAction.adoptSilently || LevelAction.noChange:
          break;
      }
      p.lastRev = c.rev;
      await _saveLevels(p);
    } catch (e) {
      debugPrint('config fetch failed: $e');
    }
  }

  List<Node> nodes = [];
  final Map<int, List<Reading>> history = {}; // node id (ascending) -> readings, oldest first
  Status? serverStatus;
  DateTime? lastOk;
  int? pingMs; // duration of the last /api/status request; null when the poll failed
  LinkState link = LinkState.connected;
  int _fails = 0;
  bool _paused = false;
  final Set<int> _historyLoaded = {};
  final Set<int> _sim = {}; // node ids currently showing the fake stream
  final Map<int, int> _simMin = {}; // minute (since epoch) of the newest fake reading
  final Map<int, double> _simLevel = {};
  final Map<int, NodeStatus> _lastStatus = {};
  final Map<int, NodeHealth> _lastHealth = {};
  bool notificationsOff = false;
  ThemeMode themeMode = ThemeMode.system;

  bool get serverTooNew => (serverStatus?.apiVersion ?? 1) > supportedApiVersion;
  /// Centre of the joined server, for an empty map.
  (double, double)? get siteCentre {
    final r = recent.where((r) => r.url == host).firstOrNull;
    return r?.lat == null || r?.lng == null ? null : (r!.lat!, r.lng!);
  }

  String? get cachedDirectory => _prefs.getString('directory');
  Future<void> saveDirectory(String json) => _prefs.setString('directory', json);

  Future<void> init() async {
    _prefs = await SharedPreferences.getInstance();
    host = _prefs.getString('host');
    themeMode = ThemeMode.values.asNameMap()[_prefs.getString('theme')] ?? ThemeMode.system;
    siteName = _prefs.getString('siteName') ?? siteName;
    recent = [
      for (final j in jsonDecode(_prefs.getString('recent') ?? '[]') as List) Site.fromJson(j as Map<String, dynamic>)
    ];
    _levels = jsonDecode(_prefs.getString('levels') ?? '{}') as Map<String, dynamic>;

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
    _cfgAt = null;
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
    if (state == AppLifecycleState.resumed) {
      checkNotifications();
      fetchConfig(force: true);
    }
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
    final a = serverStatus?.masterAgeS; // absent when the server has never reached the master: no time
    return lastOk == null || a == null ? null : lastOk!.subtract(Duration(seconds: a));
  }

  void _reset() {
    nodes = [];
    history.clear();
    serverStatus = null;
    link = LinkState.connected;
    config = null;
    levelsNote = false;
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

  Future<void> setThemeMode(ThemeMode m) async {
    themeMode = m;
    await _prefs.setString('theme', m.name);
    notifyListeners();
  }

  Node? nodeById(int id) => nodes.where((n) => n.id == id).firstOrNull;
  Reading? latestOf(int id) => history[id]!.lastOrNull;

  /// Seconds since the node's newest reading, phone clock minus server receive time.
  int? ageS(int id) {
    final t = latestOf(id)?.receivedAt;
    return t == null ? null : (DateTime.now().millisecondsSinceEpoch / 1000 - t).floor();
  }

  Future<void> poll() async {
    try {
      if (nodes.isEmpty) nodes = await _api.nodes();
      _addSimNodes();
      final latest = await _api.latest();
      for (final r in latest) {
        if (_sim.remove(r.nodeId)) { // real data arrived: drop the fake stream, load the real history
          history[r.nodeId] = [];
          _historyLoaded.remove(r.nodeId);
          _rename(r.nodeId, false);
        }
      }
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
      final sw = Stopwatch()..start();
      serverStatus = await _api.status();
      pingMs = sw.elapsedMilliseconds;
      for (final r in latest) {
        final list = history[r.nodeId];
        if (list == null || (list.isNotEmpty && !isNewReading(list.last, r))) continue;
        list.add(r);
        if (list.length > maxPoints) list.removeAt(0);
      }
      _addSim();
      _fails = 0;
      link = linkState(serverReachable: true, masterOk: serverStatus!.masterOk, siteOk: serverStatus!.siteOk);
      lastOk = DateTime.now();
      await fetchConfig();
      _checkAlerts();
    } catch (e) {
      debugPrint('poll failed (${e is TypeError || e is FormatException ? 'bad reply' : 'network'}): $e');
      _fails++;
      pingMs = null;
      link = LinkState.serverDown; // keep the last data on screen
    }
    notifyListeners();
    _schedule();
  }

  void _addSimNodes() {
    for (final e in _simNodes.entries) {
      if (nodes.every((n) => n.id != e.key)) nodes.add(Node(e.key, e.value.$1, e.value.$2, e.value.$3));
    }
    nodes.sort((a, b) => a.id.compareTo(b.id));
  }

  void _rename(int id, bool sim) {
    final i = nodes.indexWhere((n) => n.id == id);
    if (i < 0) return;
    final n = nodes[i], base = n.name.replaceAll(' (sim)', ''), d = _simNodes[id];
    nodes[i] = Node(id, sim ? '$base (sim)' : base, n.lat.isNaN && d != null ? d.$2 : n.lat, n.lng.isNaN && d != null ? d.$3 : n.lng);
  }

  /// Keeps a fake reading per minute for every node the server has nothing for.
  void _addSim() {
    final now = DateTime.now().millisecondsSinceEpoch ~/ 60000;
    for (final id in _simNodes.keys) {
      final list = history[id];
      if (list == null || (!_sim.contains(id) && list.isNotEmpty)) continue; // real data
      if (_sim.add(id)) _rename(id, true);
      var t = _simMin[id] ?? now - maxPoints, level = _simLevel[id] ?? 120.0 + id * 20;
      while (t < now) {
        t++;
        final rain = _storm((t % 180).toDouble(), (id - 1) * 0.6);
        level = (level + rain * 0.42 * (1 + id * 0.12) - level * 0.03 + (_rng.nextDouble() - .5) * 4).clamp(50.0, mountMm);
        list.add(Reading(t * 60, id, level.round(), 28 - min(6, rain * 0.08),
            min(99.0, 68 + min(25.0, rain * 0.35)), 1450 - level.round(), receivedAt: t * 60.0));
        if (list.length > maxPoints) list.removeAt(0);
      }
      _simMin[id] = now;
      _simLevel[id] = level;
    }
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
      if (r == null || _sim.contains(id)) continue;
      final name = nodeById(id)?.name ?? 'Node $id';
      final s = r.status(warnMm, dangerMm);
      if (crossedIntoAlert(_lastStatus[id], s)) {
        _show(id, 'Flood alert: $name', 'Water level ${(r.levelMm / 1000).toStringAsFixed(2)} m');
      }
      _lastStatus[id] = s;
      final h = nodeHealth(r, nowS);
      final v = lastValid(history[id]!);
      if (v != null && lostSignalFires(_lastHealth[id], h, v.levelMm, warnMm)) {
        _show(100 + id, 'Lost signal: $name',
            'No report for ${((nowS - r.receivedAt!) / 60).floor()} min. Last level ${(v.levelMm / 1000).toStringAsFixed(2)} m.');
      }
      _lastHealth[id] = h;
    }
  }
}
