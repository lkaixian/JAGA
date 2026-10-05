import 'dart:convert';
import 'dart:math';

import 'package:flutter/material.dart';
import 'package:geolocator/geolocator.dart';
import 'package:http/http.dart' as http;

import 'api.dart';
import 'main.dart' show ThemeToggle;
import 'state.dart';

/// Public list of servers, served by the first deployment (dashboard/servers.json).
const directoryUrl = 'https://jaga.larmkx.com/servers.json';
const autoJoinMaxKm = 50.0;

class Site {
  final String name, url;
  final double? lat, lng;
  Site(this.name, this.url, [this.lat, this.lng]);
  factory Site.fromJson(Map<String, dynamic> j) =>
      Site(j['name'], (j['url'] as String).replaceAll(RegExp(r'/+$'), ''), (j['lat'] as num?)?.toDouble(), (j['lng'] as num?)?.toDouble());
  Map<String, dynamic> toJson() => {'name': name, 'url': url, 'lat': lat, 'lng': lng};
}

/// Great-circle distance (haversine), km.
double distanceKm(double lat1, double lng1, double lat2, double lng2) {
  double rad(double d) => d * pi / 180;
  final a = pow(sin(rad(lat2 - lat1) / 2), 2) + cos(rad(lat1)) * cos(rad(lat2)) * pow(sin(rad(lng2 - lng1) / 2), 2);
  return 12742 * asin(sqrt(a));
}

/// Closest site with coordinates, or null if none is within [maxKm].
Site? nearestSite(List<Site> sites, double lat, double lng, {double maxKm = autoJoinMaxKm}) {
  Site? best;
  var bestKm = maxKm;
  for (final s in sites) {
    if (s.lat == null || s.lng == null) continue;
    final d = distanceKm(lat, lng, s.lat!, s.lng!);
    if (d <= bestKm) {
      best = s;
      bestKm = d;
    }
  }
  return best;
}

/// "192.168.1.5", "jaga-master.local" and host:port are plain HTTP; any other domain is HTTPS.
String normalizeUrl(String input) {
  var u = input.trim().replaceAll(RegExp(r'/+$'), '');
  if (u.startsWith('http')) return u;
  final plain = RegExp(r'^(\d+\.\d+\.\d+\.\d+|.*\.local)(:\d+)?$').hasMatch(u) || u.contains(':');
  return '${plain ? 'http' : 'https'}://$u';
}

/// Raw JSON text, so the last good copy can be cached as is.
Future<String> fetchDirectory() async {
  final res = await http.get(Uri.parse(directoryUrl)).timeout(const Duration(seconds: 6));
  if (res.statusCode != 200) throw Exception('HTTP ${res.statusCode}');
  return res.body;
}

List<Site> parseDirectory(String json) =>
    [for (final j in jsonDecode(json) as List) Site.fromJson(j as Map<String, dynamic>)];

enum LocFail { serviceOff, denied, deniedForever, failed }

/// Position or the reason there is none. Permission is asked first: the system prompt must appear
/// even when the phone's location switch is off, otherwise the user is never asked.
Future<(Position?, LocFail?)> locate() async {
  try {
    var perm = await Geolocator.checkPermission();
    if (perm == LocationPermission.denied) perm = await Geolocator.requestPermission();
    if (perm == LocationPermission.deniedForever) return (null, LocFail.deniedForever);
    if (perm == LocationPermission.denied) return (null, LocFail.denied);
    if (!await Geolocator.isLocationServiceEnabled()) return (null, LocFail.serviceOff);
    final p = await Geolocator.getCurrentPosition(
        locationSettings: const LocationSettings(accuracy: LocationAccuracy.low, timeLimit: Duration(seconds: 10)));
    return (p, null);
  } catch (_) {
    return (null, LocFail.failed);
  }
}

/// Words and the button for each failure (wording per docs/ERROR_HANDLING.md section 8 style).
(String, String) locFailText(LocFail f) => switch (f) {
      LocFail.serviceOff => ('Location is turned off on this phone.', 'Turn on location'),
      LocFail.denied => ('Location permission was declined.', 'Allow location'),
      LocFail.deniedForever => ('Location permission is blocked for this app.', 'Open app settings'),
      LocFail.failed => ("Couldn't get your location.", 'Try again'),
    };

class LobbyScreen extends StatefulWidget {
  final AppState s;
  const LobbyScreen(this.s, {super.key});
  @override
  State<LobbyScreen> createState() => _LobbyScreenState();
}

class _LobbyScreenState extends State<LobbyScreen> with WidgetsBindingObserver {
  List<Site> sites = [];
  final Map<String, bool> online = {}; // site url -> answered /api/status
  Position? pos;
  LocFail? locFail;
  String note = 'Finding servers near you…';
  bool busy = true;
  String? error;
  final address = TextEditingController();

  @override
  void initState() {
    super.initState();
    WidgetsBinding.instance.addObserver(this);
    _load(auto: widget.s.recent.isEmpty); // first launch only: auto-join the nearest
  }

  @override
  void dispose() {
    WidgetsBinding.instance.removeObserver(this);
    super.dispose();
  }

  // Back from the phone's location or app settings: look again.
  @override
  void didChangeAppLifecycleState(AppLifecycleState state) {
    if (state == AppLifecycleState.resumed && locFail != null && !busy) _load(auto: widget.s.recent.isEmpty);
  }

  Future<void> _fixLocation() async {
    switch (locFail) {
      case LocFail.serviceOff:
        await Geolocator.openLocationSettings();
      case LocFail.deniedForever:
        await Geolocator.openAppSettings();
      default:
        _load(auto: widget.s.recent.isEmpty);
    }
  }

  double? _km(Site s) =>
      pos == null || s.lat == null ? null : distanceKm(pos!.latitude, pos!.longitude, s.lat!, s.lng!);

  Future<void> _load({required bool auto}) async {
    setState(() {
      busy = true;
      error = null;
      note = 'Finding servers near you…';
    });
    final (p, fail) = await locate();
    pos = p;
    locFail = fail;
    try {
      final body = await fetchDirectory();
      sites = parseDirectory(body);
      await widget.s.saveDirectory(body);
    } catch (_) {
      try {
        sites = parseDirectory(widget.s.cachedDirectory ?? '');
        if (sites.isEmpty) throw const FormatException('empty');
        error = 'Could not load the server list. Showing the last saved list.';
      } catch (_) {
        sites = [...widget.s.recent];
        error = sites.isEmpty
            ? 'Could not load the server list. Check your connection, or join by address below.'
            : 'Could not load the server list. Showing recent servers.';
      }
    }
    sites.sort((a, b) => (_km(a) ?? double.infinity).compareTo(_km(b) ?? double.infinity));
    if (!mounted) return;
    if (auto && pos != null) {
      final near = nearestSite(sites, pos!.latitude, pos!.longitude);
      if (near != null) {
        setState(() => note = 'Joining ${near.name}…');
        if (await widget.s.join(near) == null) return;
      }
    }
    setState(() {
      busy = false;
      note = pos == null ? 'Pick a server.' : 'Servers near you';
    });
    for (final site in sites) {
      ApiClient(site.url).status().then((_) => true, onError: (_) => false).then((ok) {
        if (mounted) setState(() => online[site.url] = ok);
      });
    }
  }

  Future<void> _join(Site site) async {
    setState(() {
      busy = true;
      error = null;
    });
    final err = await widget.s.join(site);
    if (err != null && mounted) {
      setState(() {
        busy = false;
        error = err;
      });
    }
  }

  @override
  Widget build(BuildContext context) {
    final recent = widget.s.recent.where((r) => !sites.any((s) => s.url == r.url)).toList();
    return Scaffold(
      appBar: AppBar(title: const Text('JAGA servers'), actions: [
        ThemeToggle(widget.s),
        IconButton(icon: const Icon(Icons.refresh), onPressed: busy ? null : () => _load(auto: widget.s.recent.isEmpty)),
      ]),
      body: ListView(padding: const EdgeInsets.all(12), children: [
        if (busy) const LinearProgressIndicator(),
        Padding(padding: const EdgeInsets.symmetric(vertical: 8), child: Text(note)),
        if (locFail != null)
          Card(
            child: ListTile(
              leading: const Icon(Icons.location_off),
              title: Text(locFailText(locFail!).$1),
              trailing: TextButton(onPressed: busy ? null : _fixLocation, child: Text(locFailText(locFail!).$2)),
            ),
          ),
        if (error != null) Text(error!, style: TextStyle(color: Theme.of(context).colorScheme.error)),
        for (final site in [...sites, ...recent]) _tile(site),
        const Divider(height: 32),
        TextField(
          controller: address,
          keyboardType: TextInputType.url,
          decoration: const InputDecoration(
            labelText: 'Join by address',
            helperText: 'e.g. jaga.larmkx.com, or a LAN IP like 192.168.1.50',
            border: OutlineInputBorder(),
          ),
        ),
        const SizedBox(height: 8),
        FilledButton(
          onPressed: busy
              ? null
              : () {
                  final u = normalizeUrl(address.text);
                  if (address.text.trim().isNotEmpty) _join(Site(Uri.parse(u).host, u));
                },
          child: const Text('Join'),
        ),
      ]),
    );
  }

  Widget _tile(Site site) {
    final km = _km(site), ok = online[site.url];
    return Card(
      child: ListTile(
        leading: Icon(Icons.circle, size: 14, color: ok == null ? Colors.grey : (ok ? Colors.green : Colors.red)),
        title: Text(site.name),
        subtitle: Text([
          if (km != null) '${km < 10 ? km.toStringAsFixed(1) : km.toStringAsFixed(0)} km away',
          site.url.replaceFirst(RegExp(r'^https?://'), ''),
        ].join(' · ')),
        trailing: const Icon(Icons.chevron_right),
        onTap: busy ? null : () => _join(site),
      ),
    );
  }
}
