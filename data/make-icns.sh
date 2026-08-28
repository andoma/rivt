#!/bin/sh
# Regenerate data/rivt.icns from data/rivt.svg (macOS only; needs Xcode CLT).
# The result is checked in so builds don't depend on this script.
#
# The SVG tile spans 87.5% of its canvas; Apple's icon grid wants the
# rounded square at ~80.5% of the 1024pt canvas, so the SVG is drawn at
# 0.92 scale, centered, onto a transparent background.
set -e
cd "$(dirname "$0")"
ICONSET=$(mktemp -d)/rivt.iconset
mkdir -p "$ICONSET"

swift - "$PWD/rivt.svg" "$ICONSET" <<'EOF'
import AppKit

let svgPath = CommandLine.arguments[1]
let outDir = CommandLine.arguments[2]
guard let svg = NSImage(contentsOfFile: svgPath) else {
    fatalError("cannot load \(svgPath) (SVG rasterization needs macOS 11+)")
}

func write(_ px: Int, _ name: String) {
    let rep = NSBitmapImageRep(bitmapDataPlanes: nil, pixelsWide: px, pixelsHigh: px,
                               bitsPerSample: 8, samplesPerPixel: 4, hasAlpha: true,
                               isPlanar: false, colorSpaceName: .deviceRGB,
                               bytesPerRow: 0, bitsPerPixel: 0)!
    NSGraphicsContext.saveGraphicsState()
    NSGraphicsContext.current = NSGraphicsContext(bitmapImageRep: rep)
    let scale = 0.92
    let side = Double(px) * scale
    let off = (Double(px) - side) / 2
    svg.draw(in: NSRect(x: off, y: off, width: side, height: side),
             from: .zero, operation: .sourceOver, fraction: 1.0)
    NSGraphicsContext.restoreGraphicsState()
    try! rep.representation(using: .png, properties: [:])!
        .write(to: URL(fileURLWithPath: "\(outDir)/\(name).png"))
}

for pt in [16, 32, 128, 256, 512] {
    write(pt, "icon_\(pt)x\(pt)")
    write(pt * 2, "icon_\(pt)x\(pt)@2x")
}
EOF

iconutil -c icns "$ICONSET" -o rivt.icns
rm -rf "$(dirname "$ICONSET")"
echo "wrote $(pwd)/rivt.icns"
