import 'dart:convert';
import 'dart:math';

import 'package:flutter/material.dart';
import 'package:geolocator/geolocator.dart';
import 'package:http/http.dart' as http;

import 'api.dart';
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

Future<Position?> currentPosition() async {
  try {
    if (!await Geolocator.isLocationServiceEnabled()) return null;
    var perm = await Geolocator.checkPermission();
    if (perm == LocationPermission.denied) perm = await Geolocator.requestPermission();
    if (perm == LocationPermission.denied || perm == LocationPermission.deniedForever) return null;
    return await Geolocator.getCurrentPosition(
        locationSettings: const LocationSettings(accuracy: LocationAccuracy.low, timeLimit: Duration(seconds: 10)));
  } catch (_) {
    return null;
  }
}

class LobbyScreen extends StatefulWidget {
  final AppState s;
  const LobbyScreen(this.s, {super.key});
  @override
  State<LobbyScreen> createState() => _LobbyScreenState();
}

class _LobbyScreenState extends State<LobbyScreen> {
  List<Site> sites = [];
  final Map<String, bool> online = {}; // site url -> answered /api/status
  Position? pos;
  String note = 'Finding servers near you…';
  bool busy = true;
  String? error;
  final address = TextEditingController();

  @override
  void initState() {
    super.initState();
    _load(auto: widget.s.recent.isEmpty); // first launch only: auto-join the nearest
  }

  double? _km(Site s) =>
      pos == null || s.lat == null ? null : distanceKm(pos!.latitude, pos!.longitude, s.lat!, s.lng!);

  Future<void> _load({required bool auto}) async {
    setState(() {
      busy = true;
      error = null;
      note = 'Finding servers near you…';
    });
    pos = await currentPosition();
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
        error = 'Could not load the server list. Showing recent servers.';
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
      note = pos == null ? 'Location unavailable — pick a server.' : 'Servers near you';
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
        IconButton(icon: const Icon(Icons.refresh), onPressed: busy ? null : () => _load(auto: false)),
      ]),
      body: ListView(padding: const EdgeInsets.all(12), children: [
        if (busy) const LinearProgressIndicator(),
        Padding(padding: const EdgeInsets.symmetric(vertical: 8), child: Text(note)),
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
