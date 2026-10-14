// Zeichnet das App-Icon von SID-Synth und legt alle Größen in den Asset-Katalog.
//
//   swift SIDSynth/Icon/make_icon.swift
//
// Eigenes Motiv im Geist der SidStation — dunkles Gehäuse, leuchtendes Display mit den
// Wellenformen des SID, darunter „SID“. Das Logo von Elektron wird bewusst nicht benutzt.
//
// macOS bekommt die übliche Form mit Rand und Schatten, iOS ein randloses Quadrat (die
// Ecken rundet das System selbst ab).

import AppKit

let output = URL(fileURLWithPath: CommandLine.arguments.count > 1
                 ? CommandLine.arguments[1]
                 : "SIDSynth/App/Assets.xcassets/AppIcon.appiconset")

func color(_ hex: UInt32, _ alpha: CGFloat = 1) -> CGColor {
    CGColor(srgbRed: CGFloat((hex >> 16) & 0xFF) / 255, green: CGFloat((hex >> 8) & 0xFF) / 255,
            blue: CGFloat(hex & 0xFF) / 255, alpha: alpha)
}

/// Das Motiv in einem Quadrat der Kantenlänge `s`, Ursprung unten links.
func drawMotif(_ c: CGContext, _ s: CGFloat) {
    // Gehäuse: dunkles Violett, oben etwas heller
    let body = CGGradient(colorsSpace: CGColorSpace(name: CGColorSpace.sRGB),
                          colors: [color(0x3A2F7A), color(0x16122E)] as CFArray,
                          locations: [0, 1])!
    c.drawLinearGradient(body, start: CGPoint(x: 0, y: s), end: CGPoint(x: 0, y: 0), options: [])

    // Display
    let display = CGRect(x: s * 0.12, y: s * 0.40, width: s * 0.76, height: s * 0.44)
    let displayPath = CGPath(roundedRect: display, cornerWidth: s * 0.05, cornerHeight: s * 0.05, transform: nil)
    c.saveGState()
    c.addPath(displayPath)
    c.clip()
    let glass = CGGradient(colorsSpace: CGColorSpace(name: CGColorSpace.sRGB),
                           colors: [color(0x0E2A2A), color(0x061414)] as CFArray, locations: [0, 1])!
    c.drawLinearGradient(glass, start: CGPoint(x: 0, y: display.maxY), end: CGPoint(x: 0, y: display.minY), options: [])

    // Wellenformen: Puls, Sägezahn, Dreieck — die drei Oszillatoren
    let waves: [(CGFloat, [CGPoint])] = {
        let x0 = display.minX + s * 0.05, w = display.width - s * 0.10
        let h = display.height * 0.17
        func line(_ y: CGFloat, _ shape: (CGFloat) -> CGFloat) -> [CGPoint] {
            stride(from: 0.0, through: 1.0, by: 1.0 / 400).map { t in CGPoint(x: x0 + w * t, y: y + h * shape(t)) }
        }
        let periods: CGFloat = 3
        return [
            (display.minY + display.height * 0.76, line(display.minY + display.height * 0.76) { t in
                (t * periods).truncatingRemainder(dividingBy: 1) < 0.5 ? 0.5 : -0.5 }),
            (display.minY + display.height * 0.50, line(display.minY + display.height * 0.50) { t in
                (t * periods).truncatingRemainder(dividingBy: 1) - 0.5 }),
            (display.minY + display.height * 0.24, line(display.minY + display.height * 0.24) { t in
                let p = (t * periods).truncatingRemainder(dividingBy: 1)
                return p < 0.5 ? 2 * p - 0.5 : 1.5 - 2 * p }),
        ]
    }()
    let glow = [0x7CF2C8, 0x5FD8F0, 0xB9A8FF] as [UInt32]
    for (i, wave) in waves.enumerated() {
        let path = CGMutablePath()
        path.addLines(between: wave.1)
        c.setLineJoin(.round)
        c.setLineCap(.round)
        c.setShadow(offset: .zero, blur: s * 0.02, color: color(glow[i], 0.9))
        c.setStrokeColor(color(glow[i]))
        c.setLineWidth(s * 0.016)
        c.addPath(path)
        c.strokePath()
    }
    c.restoreGState()
    c.setStrokeColor(color(0x000000, 0.5))
    c.setLineWidth(s * 0.008)
    c.addPath(displayPath)
    c.strokePath()

    // „SID“
    let descriptor = NSFont.systemFont(ofSize: s * 0.21, weight: .heavy).fontDescriptor.withDesign(.rounded)!
    let font = NSFont(descriptor: descriptor, size: s * 0.21)!
    let text = NSAttributedString(string: "SID", attributes: [
        .font: font, .foregroundColor: NSColor.white, .kern: s * 0.02,
    ])
    let line = CTLineCreateWithAttributedString(text)
    let bounds = CTLineGetImageBounds(line, c)
    c.textPosition = CGPoint(x: (s - bounds.width) / 2 - bounds.minX, y: s * 0.11)
    CTLineDraw(line, c)

    // je ein Regler links und rechts der Schrift
    for x in [0.17, 0.83] {
        let knob = CGRect(x: s * x - s * 0.045, y: s * 0.17, width: s * 0.09, height: s * 0.09)
        c.setFillColor(color(0x0B0918))
        c.fillEllipse(in: knob)
        c.setStrokeColor(color(0x9C8CFF))
        c.setLineWidth(s * 0.01)
        c.strokeEllipse(in: knob.insetBy(dx: s * 0.006, dy: s * 0.006))
        c.move(to: CGPoint(x: knob.midX, y: knob.midY))
        c.addLine(to: CGPoint(x: knob.midX + s * 0.025, y: knob.midY + s * 0.025))
        c.strokePath()
    }
}

func render(size: Int, mac: Bool) -> Data {
    let s = CGFloat(size)
    let c = CGContext(data: nil, width: size, height: size, bitsPerComponent: 8, bytesPerRow: 0,
                      space: CGColorSpace(name: CGColorSpace.sRGB)!,
                      // iOS: Apple nimmt das Icon nur ohne Alphakanal an.
                      bitmapInfo: (mac ? CGImageAlphaInfo.premultipliedLast : .noneSkipLast).rawValue)!
    NSGraphicsContext.current = NSGraphicsContext(cgContext: c, flipped: false)
    if mac {
        // Raster von Apple: 824 von 1024, Ecken 185, leichter Schatten nach unten
        let inset = s * 100 / 1024
        let rect = CGRect(x: inset, y: inset, width: s - 2 * inset, height: s - 2 * inset)
        let shape = CGPath(roundedRect: rect, cornerWidth: s * 185 / 1024, cornerHeight: s * 185 / 1024, transform: nil)
        c.saveGState()
        c.setShadow(offset: CGSize(width: 0, height: -s * 0.012), blur: s * 0.03, color: color(0x000000, 0.35))
        c.addPath(shape)
        c.setFillColor(color(0x16122E))
        c.fillPath()
        c.restoreGState()
        c.saveGState()
        c.addPath(shape)
        c.clip()
        c.translateBy(x: rect.minX, y: rect.minY)
        drawMotif(c, rect.width)
        c.restoreGState()
    } else {
        drawMotif(c, s)
    }
    let image = c.makeImage()!
    return NSBitmapImageRep(cgImage: image).representation(using: .png, properties: [:])!
}

try FileManager.default.createDirectory(at: output, withIntermediateDirectories: true)
var images: [[String: String]] = [
    ["idiom": "universal", "platform": "ios", "size": "1024x1024", "filename": "ios-1024.png"],
]
try render(size: 1024, mac: false).write(to: output.appendingPathComponent("ios-1024.png"))
for points in [16, 32, 128, 256, 512] {
    for scale in [1, 2] {
        let pixels = points * scale
        let name = "mac-\(pixels).png"
        try render(size: pixels, mac: true).write(to: output.appendingPathComponent(name))
        images.append(["idiom": "mac", "size": "\(points)x\(points)", "scale": "\(scale)x", "filename": name])
    }
}
let contents: [String: Any] = ["images": images, "info": ["author": "xcode", "version": 1]]
try JSONSerialization.data(withJSONObject: contents, options: [.prettyPrinted, .sortedKeys])
    .write(to: output.appendingPathComponent("Contents.json"))
let catalog = output.deletingLastPathComponent().appendingPathComponent("Contents.json")
try Data("{\n  \"info\" : {\n    \"author\" : \"xcode\",\n    \"version\" : 1\n  }\n}\n".utf8).write(to: catalog)
print("Icon geschrieben nach \(output.path)")
