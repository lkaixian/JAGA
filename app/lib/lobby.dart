import 'dart:convert';
import 'dart:math';

import 'package:flutter/material.dart';
import 'package:geolocator/geolocator.dart';
import 'package:http/http.dart' as http;

import 'api.dart' show hm;
import 'main.dart' show ThemeToggle;
import 'state.dart';

/// Public list of servers, served by the first deployment (dashboard/servers.json).
const directoryUrl = 'https://jaga.larmkx.com/servers.json';
const autoJoinMaxKm = 50.0;

class Site {
  final String name, url;
  final double? lat, lng;
  final String? logo; // path on the server or a full URL
  final bool? online; // hub directory only; null = no information
  final int? lastSeenS;
  Site(this.name, this.url, [this.lat, this.lng, this.logo, this.online, this.lastSeenS]);
  factory Site.fromJson(Map<String, dynamic> j) => Site(j['name'], (j['url'] as String).replaceAll(RegExp(r'/+$'), ''),
      (j['lat'] as num?)?.toDouble(), (j['lng'] as num?)?.toDouble(), j['logo'] as String?,
      j['online'] as bool?, (j['last_seen_s'] as num?)?.toInt());
  Map<String, dynamic> toJson() =>
      {'name': name, 'url': url, 'lat': lat, 'lng': lng, 'logo': logo, 'online': online, 'last_seen_s': lastSeenS};
  bool get offline => online == false;
}

/// Relative logo path joined to the server url; full URLs kept.
String? resolveLogo(String serverUrl, String? logo) {
  if (logo == null || logo.isEmpty) return null;
  if (RegExp(r'^https?://').hasMatch(logo)) return logo;
  return '$serverUrl${logo.startsWith('/') ? '' : '/'}$logo';
}

/// The server's own name, logo and centre; just the host name when it doesn't answer.
Future<Site> fetchSite(String url) async {
  try {
    final res = await http.get(Uri.parse('$url/api/site')).timeout(const Duration(seconds: 5));
    if (res.statusCode != 200) throw const FormatException('not a site');
    final j = jsonDecode(res.body) as Map<String, dynamic>;
    return Site(j['name'] as String, url, (j['lat'] as num?)?.toDouble(), (j['lng'] as num?)?.toDouble(), j['logo'] as String?);
  } catch (_) {
    return Site(Uri.parse(url).host, url);
  }
}

/// Round trip to /api/health in ms; any HTTP answer counts. Null when nothing answers in 3 s.
Future<int?> pingMs(String url) async {
  final sw = Stopwatch()..start();
  try {
    await http.get(Uri.parse('$url/api/health')).timeout(const Duration(seconds: 3));
    return sw.elapsedMilliseconds;
  } catch (_) {
    return null;
  }
}

enum PingTone { good, ok, slow, offline }

PingTone pingTone(int? ms) => ms == null ? PingTone.offline : ms < 100 ? PingTone.good : ms < 300 ? PingTone.ok : PingTone.slow;
String pingLabel(int? ms) => ms == null ? 'offline' : '$ms ms';

/// Red chip for a hub site that has gone quiet.
class OfflineChip extends StatelessWidget {
  const OfflineChip({super.key});
  @override
  Widget build(BuildContext context) => const Chip(
        visualDensity: VisualDensity.compact,
        side: BorderSide(color: Colors.red),
        label: Text('site offline', style: TextStyle(color: Colors.red, fontSize: 12)),
      );
}

String lastSeenText(DateTime now, int? ageS) =>
    ageS == null ? 'Never reported' : 'Last seen ${hm(now.subtract(Duration(seconds: ageS)))}';

String _bare(String url) => url.replaceFirst(RegExp(r'^https?://'), '');
String _km1(double km) => km < 10 ? km.toStringAsFixed(1) : km.toStringAsFixed(0);

/// Address without the scheme but with any path, and whether it is encrypted (not plain http).
(String, bool) displayAddress(String url) => (_bare(url), !url.startsWith('http://'));

/// Ping chip; [loading] shows "…" until the answer arrives.
class PingChip extends StatelessWidget {
  final int? ms;
  final bool loading;
  const PingChip(this.ms, {super.key, this.loading = false});
  @override
  Widget build(BuildContext context) {
    final c = loading
        ? Colors.grey
        : switch (pingTone(ms)) {
            PingTone.good => Colors.green,
            PingTone.ok => Colors.amber.shade800,
            _ => Colors.red,
          };
    return Chip(
      visualDensity: VisualDensity.compact,
      side: BorderSide(color: c),
      label: Text(loading ? '…' : pingLabel(ms), style: TextStyle(color: c, fontSize: 12)),
    );
  }
}

/// Server logo, or the first letter of its name when there is none or it won't load.
class SiteAvatar extends StatelessWidget {
  final Site site;
  final double size;
  const SiteAvatar(this.site, {super.key, this.size = 40});
  @override
  Widget build(BuildContext context) {
    final fallback = CircleAvatar(radius: size / 2, child: Text(site.name.isEmpty ? '?' : site.name[0].toUpperCase()));
    final url = resolveLogo(site.url, site.logo);
    if (url == null) return fallback;
    return ClipOval(
      child: Image.network(url,
          width: size,
          height: size,
          fit: BoxFit.cover,
          errorBuilder: (_, _, _) => fallback,
          loadingBuilder: (_, child, p) => p == null ? child : fallback),
    );
  }
}

/// The one question before joining any server.
Future<bool> confirmJoin(BuildContext context, Site site, {double? km, int? ping}) async {
  final loc = site.lat == null || site.lng == null
      ? 'Location not listed'
      : '${site.lat!.abs().toStringAsFixed(4)}°${site.lat! >= 0 ? 'N' : 'S'}, '
          '${site.lng!.abs().toStringAsFixed(4)}°${site.lng! >= 0 ? 'E' : 'W'}${km == null ? '' : ' · ${_km1(km)} km away'}';
  final addr = displayAddress(site.url);
  return await showDialog<bool>(
        context: context,
        builder: (c) => AlertDialog(
          title: const Text('Join this network?'),
          content: Column(mainAxisSize: MainAxisSize.min, crossAxisAlignment: CrossAxisAlignment.start, children: [
            Row(children: [
              SiteAvatar(site),
              const SizedBox(width: 12),
              Expanded(child: Text(site.name, style: Theme.of(c).textTheme.titleLarge)),
            ]),
            const SizedBox(height: 12),
            Text(addr.$1),
            if (!addr.$2) const Text('This address is not encrypted.', style: TextStyle(color: Colors.red)),
            const SizedBox(height: 4),
            Text(loc),
            const SizedBox(height: 8),
            if (site.offline) ...[Text(lastSeenText(DateTime.now(), site.lastSeenS)), const OfflineChip()] else PingChip(ping),
          ]),
          actions: [
            TextButton(onPressed: () => Navigator.pop(c, false), child: const Text('Cancel')),
            FilledButton(onPressed: () => Navigator.pop(c, true), child: const Text('Join')),
          ],
        ),
      ) ??
      false;
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
  final Map<String, int?> pings = {}; // site url -> ping ms (null = offline); absent = still asking
  final Set<String> _declined = {}; // not asked again this session
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
      if (near != null && !_declined.contains(near.url)) {
        setState(() => note = 'Checking ${near.name}…');
        final ping = near.offline ? null : await pingMs(near.url);
        if (!mounted) return;
        if (await confirmJoin(context, near, km: _km(near), ping: ping)) {
          setState(() => note = 'Joining ${near.name}…');
          if (await widget.s.join(near) == null) return;
        } else {
          _declined.add(near.url);
        }
      }
    }
    setState(() {
      busy = false;
      note = pos == null ? 'Pick a server.' : 'Servers near you';
    });
    for (final site in sites.where((s) => !s.offline)) {
      pingMs(site.url).then((ms) {
        if (mounted) setState(() => pings[site.url] = ms);
      });
    }
  }

  Future<void> _join(Site site) async {
    setState(() {
      busy = true;
      error = null;
    });
    final ping = site.offline || pings.containsKey(site.url) ? pings[site.url] : await pingMs(site.url);
    if (!mounted) return;
    if (!await confirmJoin(context, site, km: _km(site), ping: ping)) {
      if (mounted) setState(() => busy = false);
      return;
    }
    final err = await widget.s.join(site);
    if (err != null && mounted) {
      setState(() {
        busy = false;
        error = err;
      });
    }
  }

  Future<void> _joinAddress(String u) async {
    setState(() {
      busy = true;
      error = null;
    });
    final site = await fetchSite(u);
    if (mounted) await _join(site);
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
            border: OutlineInputBorder(),
          ),
        ),
        const SizedBox(height: 8),
        FilledButton(
          onPressed: busy
              ? null
              : () {
                  final u = normalizeUrl(address.text);
                  if (address.text.trim().isNotEmpty) _joinAddress(u);
                },
          child: const Text('Join'),
        ),
      ]),
    );
  }

  Widget _tile(Site site) {
    final km = _km(site);
    return Card(
      child: ListTile(
        leading: SiteAvatar(site),
        title: Text(site.name),
        subtitle: Text(_bare(site.url) + (km == null ? '' : ' · ${_km1(km)} km') +
            (site.offline ? '\n${lastSeenText(DateTime.now(), site.lastSeenS)}' : '')),
        trailing: site.offline ? const OfflineChip() : PingChip(pings[site.url], loading: !pings.containsKey(site.url)),
        onTap: busy ? null : () => _join(site),
      ),
    );
  }
}
