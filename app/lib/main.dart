import 'package:fl_chart/fl_chart.dart';
import 'package:flutter/material.dart';

import 'api.dart';
import 'lobby.dart';
import 'map_view.dart';
import 'state.dart';

const _palette = [
  Color(0xFF2A7FFF), Color(0xFFE67E22), Color(0xFF8E44AD), Color(0xFF16A085),
  Color(0xFFC0392B), Color(0xFF7F8C8D), Color(0xFFD4AC0D), Color(0xFF2C3E50),
];
Color nodeColor(int id) => _palette[(id - 1) % _palette.length];
const _statusColors = {
  NodeStatus.normal: Color(0xFF0CA30C),
  NodeStatus.watch: Color(0xFFFAB219),
  NodeStatus.alert: Color(0xFFD03B3B),
  NodeStatus.error: Color(0xFF888888),
};
const _statusLabels = {
  NodeStatus.normal: 'Normal',
  NodeStatus.watch: 'Watch',
  NodeStatus.alert: 'Alert',
  NodeStatus.error: 'Sensor fault',
};

void main() async {
  WidgetsFlutterBinding.ensureInitialized();
  final state = AppState();
  await state.init();
  runApp(JagaApp(state));
}

class JagaApp extends StatelessWidget {
  final AppState state;
  const JagaApp(this.state, {super.key});

  @override
  Widget build(BuildContext context) => ListenableBuilder(
        listenable: state,
        builder: (context, _) => MaterialApp(
          title: 'JAGA',
          theme: ThemeData(colorSchemeSeed: Colors.blue, useMaterial3: true),
          darkTheme: ThemeData(colorSchemeSeed: Colors.blue, brightness: Brightness.dark, useMaterial3: true),
          themeMode: state.themeMode,
          scaffoldMessengerKey: state.messenger,
          home: Home(state),
        ),
      );
}

/// Flips between light and dark from whatever is showing now (including when the setting is "System").
class ThemeToggle extends StatelessWidget {
  final AppState s;
  const ThemeToggle(this.s, {super.key});

  @override
  Widget build(BuildContext context) {
    final dark = Theme.of(context).brightness == Brightness.dark;
    return IconButton(
      tooltip: dark ? 'Switch to light mode' : 'Switch to dark mode',
      icon: Icon(dark ? Icons.light_mode : Icons.dark_mode),
      onPressed: () => s.setThemeMode(dark ? ThemeMode.light : ThemeMode.dark),
    );
  }
}

class Home extends StatefulWidget {
  final AppState state;
  const Home(this.state, {super.key});
  @override
  State<Home> createState() => _HomeState();
}

class _HomeState extends State<Home> {
  int tab = 0;

  @override
  Widget build(BuildContext context) {
    final s = widget.state;
    return ListenableBuilder(
      listenable: s,
      builder: (context, _) {
        if (s.host == null) return LobbyScreen(s);
        final live = s.link == LinkState.connected && s.lastOk != null;
        final pages = [_StatusTab(s), _HistoryTab(s), _MapTab(s), _SettingsTab(s)];
        return Scaffold(
          appBar: AppBar(
            title: Text(s.siteName),
            actions: [
              ThemeToggle(s),
              Padding(
                padding: const EdgeInsets.only(right: 16),
                child: Center(
                  child: Text(
                    live
                        ? 'Live · ${_hms(s.lastOk!)}${s.pingMs == null ? '' : ' · ${pingLabel(s.pingMs)}'}'
                        : 'Offline${s.lastOk == null ? '' : ' · last ${_hms(s.lastOk!)}'}',
                    style: TextStyle(color: live ? _statusColors[NodeStatus.normal] : _statusColors[NodeStatus.alert]),
                  ),
                ),
              ),
            ],
          ),
          body: pages[tab],
          bottomNavigationBar: NavigationBar(
            selectedIndex: tab,
            onDestinationSelected: (i) => setState(() => tab = i),
            destinations: const [
              NavigationDestination(icon: Icon(Icons.water_drop), label: 'Status'),
              NavigationDestination(icon: Icon(Icons.show_chart), label: 'History'),
              NavigationDestination(icon: Icon(Icons.map), label: 'Map'),
              NavigationDestination(icon: Icon(Icons.settings), label: 'Settings'),
            ],
          ),
        );
      },
    );
  }
}

String _hms(DateTime t) => t.toIso8601String().substring(11, 19);
String _hm(DateTime t) => _hms(t).substring(0, 5);

String _age(int? s) {
  if (s == null) return '';
  if (s < 60) return '${s < 0 ? 0 : s}s ago';
  return s < 3600 ? '${s ~/ 60}m ago' : '${s ~/ 3600}h ago';
}

class _StatusTab extends StatelessWidget {
  final AppState s;
  const _StatusTab(this.s);

  @override
  Widget build(BuildContext context) => ListView(
        padding: const EdgeInsets.all(12),
        children: [
          if (s.link != LinkState.connected || s.serverTooNew) _LinkBanner(s),
          for (final id in s.history.keys) _NodeCard(s, id),
        ],
      );
}

class _LinkBanner extends StatelessWidget {
  final AppState s;
  const _LinkBanner(this.s);

  @override
  Widget build(BuildContext context) {
    final site = s.link == LinkState.siteDown;
    final down = s.link == LinkState.serverDown || site;
    final at = down ? s.lastOk : s.masterLastAt;
    final text = s.serverTooNew
        ? tooNewMsg
        : site
        ? siteOfflineText(DateTime.now(), s.serverStatus?.siteAgeS)
        : down
        ? "Can't reach the server.${at == null ? '' : ' Showing data from ${_hm(at)}.'}"
        : "The server can't reach the master node.${at == null ? '' : ' Readings stopped at ${_hm(at)}.'}";
    final c = Theme.of(context).colorScheme;
    return Card(
      color: down ? c.errorContainer : c.tertiaryContainer,
      child: Padding(
        padding: const EdgeInsets.all(12),
        child: Text(text, style: TextStyle(color: down ? c.onErrorContainer : c.onTertiaryContainer)),
      ),
    );
  }
}

class _NodeCard extends StatelessWidget {
  final AppState s;
  final int id;
  const _NodeCard(this.s, this.id);

  @override
  Widget build(BuildContext context) {
    final r = s.latestOf(id);
    final name = s.nodeById(id)?.name ?? 'Node $id';
    final st = r?.status(s.warnMm, s.dangerMm);
    final nowS = DateTime.now().millisecondsSinceEpoch / 1000; // receivedAt is Unix time; the master's now_s is not
    final health = nodeHealth(r, nowS);
    final note = switch (health) {
      NodeHealth.fault => "$name's water sensor isn't reading. Check the sonar wiring.",
      NodeHealth.noSignal =>
        "$name hasn't reported for ${((nowS - r!.receivedAt!) / 60).floor()} min.${r.sonarOk ? ' Last level ${(r.levelMm / 1000).toStringAsFixed(2)} m.' : ''}",
      _ => null,
    };
    final card = Card(
      child: Padding(
        padding: const EdgeInsets.all(16),
        child: Column(crossAxisAlignment: CrossAxisAlignment.start, children: [
          Row(children: [
            CircleAvatar(radius: 6, backgroundColor: nodeColor(id)),
            const SizedBox(width: 8),
            Expanded(child: Text(name, style: Theme.of(context).textTheme.titleMedium)),
            if (st != null) Chip(label: Text(_statusLabels[st]!), backgroundColor: _statusColors[st]!.withValues(alpha: 0.2)),
          ]),
          if (r == null)
            const Text('No data yet')
          else ...[
            Text(r.sonarOk ? '${(r.levelMm / 1000).toStringAsFixed(2)} m' : '—',
                style: Theme.of(context).textTheme.displaySmall),
            Text([
              r.tempC == null ? 'temp —' : '${r.tempC!.toStringAsFixed(1)} °C',
              r.humidityPct == null ? 'humidity —' : '${r.humidityPct!.toStringAsFixed(0)} %',
              r.distanceMm == null ? 'distance —' : '${r.distanceMm} mm',
            ].join('  ·  ')),
            if (note != null) Text(note, style: TextStyle(color: Theme.of(context).colorScheme.error)),
            Text('node $id${s.ageS(id) == null ? '' : ' · logged ${_age(s.ageS(id))}'}', style: Theme.of(context).textTheme.bodySmall),
          ],
        ]),
      ),
    );
    return health == NodeHealth.noSignal ? Opacity(opacity: 0.6, child: card) : card;
  }
}

class _HistoryTab extends StatelessWidget {
  final AppState s;
  const _HistoryTab(this.s);

  @override
  Widget build(BuildContext context) {
    HorizontalLine line(double y, NodeStatus st) => HorizontalLine(
        y: y, color: _statusColors[st], strokeWidth: 1.5, dashArray: [4, 4]);
    return Padding(
      padding: const EdgeInsets.all(16),
      child: Column(children: [
        Wrap(spacing: 12, children: [
          for (final id in s.history.keys)
            Chip(
              avatar: CircleAvatar(backgroundColor: nodeColor(id)),
              label: Text(s.nodeById(id)?.name ?? 'Node $id'),
            ),
        ]),
        Expanded(
          child: LineChart(LineChartData(
            minY: 0,
            maxY: s.mountMm,
            titlesData: FlTitlesData(
              topTitles: const AxisTitles(),
              rightTitles: const AxisTitles(),
              bottomTitles: const AxisTitles(),
              leftTitles: AxisTitles(
                sideTitles: SideTitles(
                  showTitles: true,
                  reservedSize: 40,
                  interval: 200,
                  getTitlesWidget: (v, _) => Text((v / 1000).toStringAsFixed(1)),
                ),
              ),
            ),
            gridData: const FlGridData(drawVerticalLine: false),
            borderData: FlBorderData(show: false),
            extraLinesData: ExtraLinesData(horizontalLines: [
              line(s.warnMm, NodeStatus.watch),
              line(s.dangerMm, NodeStatus.alert),
            ]),
            lineBarsData: [
              for (final e in s.history.entries)
                LineChartBarData(
                  color: nodeColor(e.key),
                  dotData: const FlDotData(show: false),
                  // x = how many readings back from the newest, so nodes with
                  // different lengths still line up at the right edge
                  spots: [
                    for (var i = 0; i < e.value.length; i++)
                      if (e.value[i].sonarOk)
                        FlSpot((i - e.value.length + maxPoints).toDouble(), e.value[i].levelMm.toDouble()),
                  ],
                ),
            ],
            minX: 0,
            maxX: maxPoints.toDouble(),
          )),
        ),
        Text('Last $maxPoints readings (m). Dashed: warning / danger.', style: Theme.of(context).textTheme.bodySmall),
      ]),
    );
  }
}

/// "Level 0.82 m · Normal" style line for the map pin sheet.
String _nodeInfo(AppState s, int id) {
  final r = s.latestOf(id);
  if (r == null) return 'No data yet';
  final h = nodeHealth(r, DateTime.now().millisecondsSinceEpoch / 1000);
  final health = switch (h) {
    NodeHealth.noSignal => 'No signal',
    NodeHealth.fault => 'Sensor fault',
    _ => _statusLabels[r.status(s.warnMm, s.dangerMm)]!,
  };
  return '${r.sonarOk ? 'Level ${(r.levelMm / 1000).toStringAsFixed(2)} m' : 'Level unknown'} · $health';
}

class _MapTab extends StatelessWidget {
  final AppState s;
  const _MapTab(this.s);

  @override
  Widget build(BuildContext context) => Padding(
        padding: const EdgeInsets.all(12),
        child: MapView(
          nodes: s.nodes,
          colors: {
            for (final id in s.history.keys)
              id: _statusColors[s.latestOf(id)?.status(s.warnMm, s.dangerMm)] ?? Colors.grey,
          },
          info: {
            for (final id in s.history.keys) id: _nodeInfo(s, id),
          },
          fallback: s.siteCentre,
        ),
      );
}

class _SettingsTab extends StatefulWidget {
  final AppState s;
  const _SettingsTab(this.s);
  @override
  State<_SettingsTab> createState() => _SettingsTabState();
}

class _SettingsTabState extends State<_SettingsTab> {
  @override
  void initState() {
    super.initState();
    widget.s.checkNotifications();
  }

  late final warn = TextEditingController(text: widget.s.levels.$1.toString());
  late final danger = TextEditingController(text: widget.s.levels.$2.toString());
  late final depth = TextEditingController(text: widget.s.levels.$3.toString());
  String? error;
  late final bool _note = widget.s.levelsNote; // opening Settings once dismisses the note

  Widget _field(String label, TextEditingController c) => Padding(
        padding: const EdgeInsets.only(bottom: 12),
        child: TextField(
          controller: c,
          keyboardType: TextInputType.number,
          decoration: InputDecoration(labelText: label, border: const OutlineInputBorder()),
        ),
      );

  Widget _published(AppState s) {
    final c = s.config;
    final (w, d, t) = c?.levels ?? defaultLevels;
    Widget row(String a, int v) => ListTile(dense: true, title: Text(a), trailing: Text('$v mm'));
    return Column(crossAxisAlignment: CrossAxisAlignment.start, children: [
      Text(c == null ? 'Not published by this server' : 'Published by ${s.siteName}',
          style: Theme.of(context).textTheme.titleSmall),
      Opacity(opacity: c == null ? 0.5 : 1, child: Column(children: [row('Warning', w), row('Danger', d), row('Chart top', t)])),
    ]);
  }

  void _save() {
    final w = int.tryParse(warn.text.trim()), d = int.tryParse(danger.text.trim()), t = int.tryParse(depth.text.trim());
    final e = validateLevels(w, d, t);
    setState(() => error = e);
    if (e != null) return;
    widget.s.saveCustom((w!, d!, t!));
    ScaffoldMessenger.of(context).showSnackBar(const SnackBar(content: Text('Saved')));
  }

  @override
  Widget build(BuildContext context) => ListView(padding: const EdgeInsets.all(16), children: [
        if (widget.s.notificationsOff)
          Card(
            color: Theme.of(context).colorScheme.errorContainer,
            child: const ListTile(
              leading: Icon(Icons.notifications_off),
              title: Text('Alerts are off. Allow notifications to get flood alerts.'),
            ),
          ),
        Card(
          child: ListTile(
            leading: const Icon(Icons.dns),
            title: Text(widget.s.siteName),
            subtitle: Text(widget.s.host ?? ''),
            trailing: TextButton(onPressed: widget.s.leave, child: const Text('Switch server')),
          ),
        ),
        const SizedBox(height: 12),
        SegmentedButton<ThemeMode>(
          segments: const [
            ButtonSegment(value: ThemeMode.system, label: Text('System'), icon: Icon(Icons.brightness_auto)),
            ButtonSegment(value: ThemeMode.light, label: Text('Light'), icon: Icon(Icons.light_mode)),
            ButtonSegment(value: ThemeMode.dark, label: Text('Dark'), icon: Icon(Icons.dark_mode)),
          ],
          selected: {widget.s.themeMode},
          onSelectionChanged: (v) => widget.s.setThemeMode(v.first),
        ),
        const SizedBox(height: 12),
        Text('Water level alerts', style: Theme.of(context).textTheme.titleMedium),
        _published(widget.s),
        if (_note || widget.s.levelsNote) const Text("The server's levels changed. Yours are kept."),
        SwitchListTile(
          contentPadding: EdgeInsets.zero,
          title: const Text('Use my own levels'),
          value: widget.s.levelPrefs.custom,
          onChanged: (v) {
            widget.s.setCustom(v);
            warn.text = widget.s.levels.$1.toString();
            danger.text = widget.s.levels.$2.toString();
            depth.text = widget.s.levels.$3.toString();
            setState(() => error = null);
          },
        ),
        if (widget.s.levelPrefs.custom) ...[
          _field('Warning at (mm)', warn),
          _field('Danger at (mm)', danger),
          _field('Chart top (mm)', depth),
          if (error != null) Padding(padding: const EdgeInsets.only(bottom: 8), child: Text(error!, style: TextStyle(color: Theme.of(context).colorScheme.error))),
          FilledButton(onPressed: _save, child: const Text('Save')),
        ],
        SwitchListTile(
          contentPadding: EdgeInsets.zero,
          title: const Text('Overwrite protection'),
          subtitle: const Text('On: the server never changes your levels. Off: your levels are replaced when the server publishes new ones.'),
          value: widget.s.levelPrefs.protect,
          onChanged: widget.s.levelPrefs.custom ? widget.s.setProtect : null,
        ),
        const SizedBox(height: 16),
        const Text('Alerts only fire while the app is running.'),
      ]);
}
