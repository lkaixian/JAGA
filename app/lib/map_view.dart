import 'package:flutter/material.dart';

import 'api.dart';

/// Schematic (not geographic) map: node pins projected from lat/lng. Draws
/// offline, unlike a tile map.
class MapView extends StatelessWidget {
  final List<Node> nodes;
  final Map<int, Color> colors; // node id -> status ring color
  const MapView({super.key, required this.nodes, required this.colors});

  @override
  Widget build(BuildContext context) => CustomPaint(
        size: Size.infinite,
        painter: _MapPainter(nodes, colors, Theme.of(context)),
      );
}

class _MapPainter extends CustomPainter {
  final List<Node> nodes;
  final Map<int, Color> colors;
  final ThemeData theme;
  _MapPainter(this.nodes, this.colors, this.theme);

  @override
  void paint(Canvas canvas, Size size) {
    if (nodes.isEmpty) return;
    const pad = 48.0;
    double lo(Iterable<double> v) => v.reduce((a, b) => a < b ? a : b);
    double hi(Iterable<double> v) => v.reduce((a, b) => a > b ? a : b);
    final lats = nodes.map((n) => n.lat), lngs = nodes.map((n) => n.lng);
    final latSpan = (hi(lats) - lo(lats)).clamp(1e-4, 1.0), lngSpan = (hi(lngs) - lo(lngs)).clamp(1e-4, 1.0);
    Offset at(Node n) => Offset(
          pad + (n.lng - lo(lngs)) / lngSpan * (size.width - 2 * pad),
          pad + (1 - (n.lat - lo(lats)) / latSpan) * (size.height - 2 * pad),
        );

    final pts = nodes.map(at).toList();
    final river = Path()..moveTo(pts.first.dx, pts.first.dy);
    for (final p in pts.skip(1)) {
      river.lineTo(p.dx, p.dy);
    }
    canvas.drawPath(
        river,
        Paint()
          ..color = theme.colorScheme.primary.withValues(alpha: 0.4)
          ..style = PaintingStyle.stroke
          ..strokeWidth = 8
          ..strokeCap = StrokeCap.round
          ..strokeJoin = StrokeJoin.round);

    for (var i = 0; i < nodes.length; i++) {
      final n = nodes[i], p = pts[i];
      canvas.drawCircle(p, 11, Paint()..color = colors[n.id] ?? Colors.grey);
      canvas.drawCircle(p, 5, Paint()..color = theme.colorScheme.surface);
      final tp = TextPainter(
        text: TextSpan(
            text: '${n.name}\n${n.lat.toStringAsFixed(4)}, ${n.lng.toStringAsFixed(4)}',
            style: TextStyle(color: theme.colorScheme.onSurface, fontSize: 11)),
        textDirection: TextDirection.ltr,
      )..layout(maxWidth: 150);
      final x = (p.dx + 16 + tp.width > size.width) ? p.dx - 16 - tp.width : p.dx + 16;
      tp.paint(canvas, Offset(x, p.dy - tp.height / 2));
    }
  }

  @override
  bool shouldRepaint(_MapPainter old) => true;
}
