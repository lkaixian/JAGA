import 'dart:convert';

import 'package:flutter/material.dart';
import 'package:flutter/services.dart' show rootBundle;
import 'package:flutter_map/flutter_map.dart';
import 'package:latlong2/latlong.dart';

import 'api.dart';

/// OpenStreetMap with a pin per node and the river lines (assets/rivers.json).
class MapView extends StatefulWidget {
  final List<Node> nodes;
  final Map<int, Color> colors; // node id -> status ring color
  final Map<int, String> info; // node id -> "Level 0.82 m · Normal"
  final (double, double)? fallback; // server centre, used when no node has coordinates
  const MapView({super.key, required this.nodes, required this.colors, this.info = const {}, this.fallback});

  @override
  State<MapView> createState() => _MapViewState();
}

class _MapViewState extends State<MapView> {
  List<Polyline> rivers = [];
  bool tileError = false;

  @override
  void initState() {
    super.initState();
    rootBundle.loadString('assets/rivers.json').then((t) {
      if (!mounted) return;
      setState(() => rivers = [
            for (final r in jsonDecode(t) as List)
              Polyline(
                points: [for (final p in r['p'] as List) LatLng((p[0] as num).toDouble(), (p[1] as num).toDouble())],
                color: const Color(0xFF0B5CD5).withValues(alpha: 0.85),
                strokeWidth: 4,
              ),
          ]);
    });
  }

  void _sheet(Node n) => showModalBottomSheet<void>(
        context: context,
        builder: (_) => ListTile(
          leading: CircleAvatar(backgroundColor: widget.colors[n.id] ?? Colors.grey, child: Text('${n.id}')),
          title: Text(n.name),
          subtitle: Text(widget.info[n.id] ?? 'No data yet'),
        ),
      );

  @override
  Widget build(BuildContext context) {
    final placed = widget.nodes.where((n) => n.lat.isFinite && n.lng.isFinite).toList();
    final pts = [for (final n in placed) LatLng(n.lat, n.lng)];
    final fb = widget.fallback;
    final options = MapOptions(
      initialCenter: pts.length == 1 ? pts.first : (fb == null ? const LatLng(1.865, 103.086) : LatLng(fb.$1, fb.$2)),
      initialZoom: pts.length == 1 ? 16 : 15,
      initialCameraFit: pts.length > 1
          ? CameraFit.bounds(bounds: LatLngBounds.fromPoints(pts), padding: const EdgeInsets.all(40), maxZoom: 17)
          : null,
    );
    return Column(children: [
      Expanded(
        child: Stack(children: [
          FlutterMap(
            key: ValueKey(pts.length), // nodes arrive after the first poll: rebuild to refit
            options: options,
            children: [
              TileLayer(
                urlTemplate: 'https://tile.openstreetmap.org/{z}/{x}/{y}.png',
                userAgentPackageName: 'my.jaga.jaga_app',
                errorTileCallback: (tile, error, stack) {
                  if (tileError) return;
                  WidgetsBinding.instance.addPostFrameCallback((_) {
                    if (mounted) setState(() => tileError = true);
                  });
                },
              ),
              PolylineLayer(polylines: rivers),
              MarkerLayer(markers: [
                for (final n in placed)
                  Marker(
                    point: LatLng(n.lat, n.lng),
                    width: 28,
                    height: 28,
                    child: GestureDetector(
                      onTap: () => _sheet(n),
                      child: Container(
                        alignment: Alignment.center,
                        decoration: BoxDecoration(
                          shape: BoxShape.circle,
                          color: const Color(0xFF263238),
                          border: Border.all(color: widget.colors[n.id] ?? Colors.grey, width: 4),
                        ),
                        child: Text('${n.id}', style: const TextStyle(color: Colors.white, fontSize: 11, fontWeight: FontWeight.bold)),
                      ),
                    ),
                  ),
              ]),
            ],
          ),
          Positioned(
            right: 0,
            bottom: 0,
            child: Container(
              color: Colors.white70,
              padding: const EdgeInsets.symmetric(horizontal: 4, vertical: 1),
              child: const Text('© OpenStreetMap contributors', style: TextStyle(fontSize: 10, color: Colors.black87)),
            ),
          ),
        ]),
      ),
      if (tileError)
        const Padding(
          padding: EdgeInsets.only(top: 8),
          child: Text("Map tiles couldn't load. Pins still show where the nodes are."),
        ),
    ]);
  }
}
