import 'package:fl_chart/fl_chart.dart';
import 'package:flutter/material.dart';

import 'api.dart';
import 'lobby.dart';
import 'map_view.dart';
import 'state.dart';

const _nodeColors = {1: Color(0xFF2A7FFF), 2: Color(0xFFE67E22), 3: Color(0xFF8E44AD)};
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
  Widget build(BuildContext context) => MaterialApp(
        title: 'JAGA',
        theme: ThemeData(colorSchemeSeed: Colors.blue, useMaterial3: true),
        darkTheme: ThemeData(colorSchemeSeed: Colors.blue, brightness: Brightness.dark, useMaterial3: true),
        home: Home(state),
      );
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
              Padding(
                padding: const EdgeInsets.only(right: 16),
                child: Center(
                  child: Text(
                    live
                        ? 'Live · ${_hms(s.lastOk!)}'
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
          if (s.link != LinkState.connected) _LinkBanner(s),
          for (final id in s.history.keys) _NodeCard(s, id),
        ],
      );
}

class _LinkBanner extends StatelessWidget {
  final AppState s;
  const _LinkBanner(this.s);

  @override
  Widget build(BuildContext context) {
    final down = s.link == LinkState.serverDown;
    final at = down ? s.lastOk : s.masterLastAt;
    final text = down
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
            CircleAvatar(radius: 6, backgroundColor: _nodeColors[id]),
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
            Text('node $id · logged ${_age(s.ageS(id))}', style: Theme.of(context).textTheme.bodySmall),
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
              avatar: CircleAvatar(backgroundColor: _nodeColors[id]),
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
                  color: _nodeColors[e.key],
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

class _MapTab extends StatelessWidget {
  final AppState s;
  const _MapTab(this.s);

  @override
  Widget build(BuildContext context) => Padding(
        padding: const EdgeInsets.all(12),
        child: MapView(nodes: s.nodes, colors: {
          for (final id in s.history.keys)
            id: _statusColors[s.latestOf(id)?.status(s.warnMm, s.dangerMm)] ?? Colors.grey,
        }),
      );
}

class _SettingsTab extends StatefulWidget {
  final AppState s;
  const _SettingsTab(this.s);
  @override
  State<_SettingsTab> createState() => _SettingsTabState();
}

class _SettingsTabState extends State<_SettingsTab> {
  late final warn = TextEditingController(text: widget.s.warnMm.toStringAsFixed(0));
  late final danger = TextEditingController(text: widget.s.dangerMm.toStringAsFixed(0));
  late final mount = TextEditingController(text: widget.s.mountMm.toStringAsFixed(0));

  Widget _field(String label, TextEditingController c, {bool number = true, String? help}) => Padding(
        padding: const EdgeInsets.only(bottom: 12),
        child: TextField(
          controller: c,
          keyboardType: number ? TextInputType.number : TextInputType.url,
          decoration: InputDecoration(labelText: label, helperText: help, border: const OutlineInputBorder()),
        ),
      );

  @override
  Widget build(BuildContext context) => ListView(padding: const EdgeInsets.all(16), children: [
        Card(
          child: ListTile(
            leading: const Icon(Icons.dns),
            title: Text(widget.s.siteName),
            subtitle: Text(widget.s.host ?? ''),
            trailing: TextButton(onPressed: widget.s.leave, child: const Text('Switch server')),
          ),
        ),
        const SizedBox(height: 12),
        _field('Warning at (mm)', warn),
        _field('Danger at (mm)', danger),
        _field('Mount height (mm, chart top)', mount),
        FilledButton(
          onPressed: () {
            widget.s.saveSettings(
              double.tryParse(warn.text) ?? widget.s.warnMm,
              double.tryParse(danger.text) ?? widget.s.dangerMm,
              double.tryParse(mount.text) ?? widget.s.mountMm,
            );
            ScaffoldMessenger.of(context).showSnackBar(const SnackBar(content: Text('Saved')));
          },
          child: const Text('Save'),
        ),
        const SizedBox(height: 16),
        const Text('Alerts only fire while the app is running.'),
      ]);
}
